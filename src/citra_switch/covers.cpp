// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <curl/curl.h>
#include <sys/stat.h>

#include "citra_switch/camera/image_decode.h"
#include "citra_switch/config.h"
#include "citra_switch/cover_share_hooks.h"
#include "citra_switch/covers.h"
#include "common/logging/log.h"

namespace SwitchFrontend {
namespace {

constexpr std::size_t MAX_CACHED = 28;

struct CacheEntry {
    std::shared_ptr<const CoverArt> art; // Null: looked, found nothing.
    std::uint64_t last_used{};
};

std::unordered_map<std::string, CacheEntry> s_cache;
std::uint64_t s_clock = 0;
int s_seen_generation = 0;

std::atomic<int> s_source{static_cast<int>(CoverSource::GameTdb)};
std::atomic<int> s_generation{0};

std::thread s_thread;
std::atomic<bool> s_active{false};
std::atomic<bool> s_cancel{false};
std::atomic<int> s_done{0};
std::atomic<int> s_total{0};
#if defined(__SWITCH2__)
std::atomic<int> s_quality{1};
#else
std::atomic<int> s_quality{0}; // Switch 1: less memory and a slower card.
#endif
std::atomic<bool> s_flush_all{false}; // Covers were replaced: reload the loaded ones too.
std::atomic<bool> s_overwrite{false};
std::atomic<bool> s_covers_shown{true};
std::atomic<int> s_forward_view{0};
std::mutex s_status_mutex;
std::string s_status_brief;
std::string s_status_detail;

std::atomic<bool> s_shelf_arrows{true};
std::atomic<bool> s_shelf_title{true};
std::atomic<int> s_shelf_bar{0};

std::string CoversDir() {
    return GetPaths().user_dir + "/covers";
}

bool FileExists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string TitleIdText(std::uint64_t program_id) {
    char buffer[20];
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(program_id));
    return buffer;
}

/// The first cover file there is for `id`, wraps first. `full` says which kind it was.
std::string FindCoverFile(const std::string& id, bool& full) {
    if (id.empty()) {
        return {};
    }
    const std::string dir = CoversDir();
    for (const bool wrap : {true, false}) {
        for (const char* extension : {".jpg", ".png", ".jpeg"}) {
            std::string path = dir + (wrap ? "/full/" : "/") + id + extension;
            if (FileExists(path)) {
                full = wrap;
                return path;
            }
        }
    }
    return {};
}

std::shared_ptr<const CoverArt> LoadCoverFile(const std::string& path, bool full) {
    DecodedImage decoded;
    // Big enough for the selected cover at full size; larger files are scaled down on load.
    const std::string error = DecodeImageFile(path, full ? 900 : 440, 400, decoded);
    if (!error.empty() || decoded.width <= 0 || decoded.height <= 0) {
        LOG_WARNING(Frontend, "Cover {} could not be read: {}", path, error);
        return nullptr;
    }
    auto art = std::make_shared<CoverArt>();
    art->image.w = decoded.width;
    art->image.h = decoded.height;
    art->image.px.resize(static_cast<std::size_t>(decoded.width) * decoded.height);
    std::memcpy(art->image.px.data(), decoded.rgba.data(),
                std::min(decoded.rgba.size(), art->image.px.size() * 4));
    for (std::uint32_t& pixel : art->image.px) {
        pixel |= 0xFF000000u; // Covers are opaque.
    }
    // A wrap is far wider than tall; a front is close to square. Trust the picture over the
    // folder it was found in, so a front dropped into covers/full still looks right.
    art->full_wrap = decoded.width > decoded.height * 17 / 10;
    (void)full;
    return art;
}

void EvictIfNeeded() {
    while (s_cache.size() > MAX_CACHED) {
        auto oldest = s_cache.begin();
        for (auto it = s_cache.begin(); it != s_cache.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) {
                oldest = it;
            }
        }
        s_cache.erase(oldest);
    }
}

std::size_t WriteFile(char* data, std::size_t size, std::size_t count, void* userdata) {
    return std::fwrite(data, size, count, static_cast<std::FILE*>(userdata)) * size;
}

int AbortWhenCancelled(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return s_cancel.load() ? 1 : 0;
}

enum class Fetch { Ok, NotThere, Failed };

/// Fetches `url` into `path`. The file only appears once the download is complete. `error`
/// gets libcurl's words when the transfer itself failed (as opposed to the site saying no).
Fetch Download(const std::string& url, const std::string& path, std::string& error) {
    const std::string partial = path + ".part";
    std::FILE* file = std::fopen(partial.c_str(), "wb");
    if (file == nullptr) {
        error = "cannot write to the covers folder";
        return Fetch::Failed;
    }
    CURL* curl = curl_easy_init();
    CURLcode code = CURLE_FAILED_INIT;
    char message[CURL_ERROR_SIZE]{};
    if (curl != nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "covers/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, AbortWhenCancelled);
        code = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    const long size = std::ftell(file);
    std::fclose(file);
    if (code == CURLE_OK && size > 1024) {
        std::remove(path.c_str());
        return std::rename(partial.c_str(), path.c_str()) == 0 ? Fetch::Ok : Fetch::Failed;
    }
    std::remove(partial.c_str());
    if (code == CURLE_OK || code == CURLE_HTTP_RETURNED_ERROR) {
        return Fetch::NotThere; // The site answered: it has no such picture.
    }
    error = message[0] != '\0' ? message : curl_easy_strerror(code);
    LOG_WARNING(Frontend, "Covers: {} failed: {} ({})", url, error, static_cast<int>(code));
    return Fetch::Failed;
}

/// GameTDB files its art by region; the last letter of the code says which to try first.
std::vector<const char*> RegionsFor(const std::string& code) {
    switch (code.empty() ? ' ' : code.back()) {
    case 'J':
        return {"JA", "US", "EN"};
    case 'P':
    case 'X':
    case 'Y':
    case 'Z':
    case 'D':
    case 'F':
    case 'S':
    case 'I':
        return {"EN", "US", "JA"};
    case 'K':
        return {"KO", "US", "EN"};
    default:
        return {"US", "EN", "JA"};
    }
}

void SetStatus(std::string brief, std::string detail) {
    std::lock_guard lock{s_status_mutex};
    s_status_brief = std::move(brief);
    s_status_detail = std::move(detail);
}

void DownloadThread(std::vector<CoverRequest> requests) {
    const std::string dir = CoversDir();
    mkdir(dir.c_str(), 0777);
    mkdir((dir + "/full").c_str(), 0777);

    struct Kind {
        const char* folder; // On GameTDB.
        bool wrap;
    };
    // The whole wrap if GameTDB has it, the front otherwise; large files before small ones
    // when those are wanted.
    std::vector<Kind> kinds;
    const bool large = s_quality.load() != 0;
    if (large) {
        kinds.push_back({"coverfullHQ", true});
    }
    kinds.push_back({"coverfull", true});
    if (large) {
        kinds.push_back({"coverM", false});
    }
    kinds.push_back({"cover", false});

    // The site answers over both; a console whose clock or certificate store is off cannot
    // do the secure one, and box art is not worth refusing over.
    const char* scheme = "https";
    int got_count = 0;
    std::string failure;
    for (const CoverRequest& request : requests) {
        if (s_cancel.load() || !failure.empty()) {
            break;
        }
        const std::string& code = request.game_code;
        bool got = false;
        // The same game under its other regions' codes, after its own: a ROM marked
        // region-free (AT3A) often has only a front on GameTDB while the American or
        // European release (AT3E, AT3P) has the whole wrap.
        std::vector<std::string> ids{code};
        for (const char letter : {'E', 'P', 'J'}) {
            if (code.back() != letter) {
                ids.push_back(code.substr(0, 3) + letter);
            }
        }
        for (const Kind& kind : kinds) {
            for (const std::string& id : ids) {
              for (const char* region : RegionsFor(id)) {
                if (got || s_cancel.load() || !failure.empty()) {
                    break;
                }
                // A front is only worth taking under the game's own code.
                if (!kind.wrap && id != code) {
                    break;
                }
                const std::string tail = std::string{"://art.gametdb.com/3ds/"} + kind.folder +
                                         "/" + region + "/" + id + ".jpg";
                const std::string path = dir + (kind.wrap ? "/full/" : "/") + code + ".jpg";
                std::string error;
                Fetch result = Download(scheme + tail, path, error);
                if (result == Fetch::Failed && std::strcmp(scheme, "https") == 0 &&
                    !s_cancel.load()) {
                    LOG_INFO(Frontend, "Covers: secure connection failed, trying plain http");
                    scheme = "http";
                    result = Download(scheme + tail, path, error);
                }
                if (result == Fetch::Failed && !s_cancel.load()) {
                    failure = error; // Neither way works: no point trying the rest.
                }
                got = result == Fetch::Ok;
              }
            }
            if (got) {
                break;
            }
        }
        if (got) {
            ++got_count;
            if (s_overwrite.load()) {
                s_flush_all = true;
            }
            ++s_generation;
        }
        ++s_done;
        SetStatus(std::to_string(s_done.load()) + " / " + std::to_string(s_total.load()),
                  "Downloading covers");
    }
    const int total = s_total.load();
    if (!failure.empty()) {
        SetStatus("No connection", "Covers could not be downloaded: " + failure);
    } else if (s_cancel.load()) {
        SetStatus("Stopped", "The cover download was stopped.");
    } else if (got_count == 0) {
        SetStatus("None found", "GameTDB has no cover for the " + std::to_string(total) +
                                    (total == 1 ? " game" : " games") + " still without one.");
    } else {
        SetStatus(std::to_string(got_count) + " new",
                  std::to_string(got_count) + (got_count == 1 ? " cover" : " covers") +
                      " downloaded" +
                      (got_count < total
                           ? "; GameTDB has none for the other " +
                                 std::to_string(total - got_count) + "."
                           : "."));
    }
    if (got_count > 0) {
        if (std::FILE* file = std::fopen((dir + "/quality.txt").c_str(), "w")) {
            std::fputs(large ? "1\n" : "0\n", file);
            std::fclose(file);
        }
    }
    LOG_INFO(Frontend, "Covers: {} of {} downloaded{}{}", got_count, total,
             failure.empty() ? "" : ", stopped by: ", failure);
    s_active = false;
}

} // namespace

std::string ReadGameCode(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::string code;
    char magic[4]{};
    long base = -1;
    if (std::fseek(file, 0x100, SEEK_SET) == 0 && std::fread(magic, 1, 4, file) == 4) {
        if (std::memcmp(magic, "NCCH", 4) == 0) {
            base = 0;
        } else if (std::memcmp(magic, "NCSD", 4) == 0) {
            // Offset of the first partition, in 0x200-byte media units.
            std::uint8_t raw[4]{};
            if (std::fseek(file, 0x120, SEEK_SET) == 0 && std::fread(raw, 1, 4, file) == 4) {
                const std::uint32_t units = raw[0] | (raw[1] << 8) | (raw[2] << 16) |
                                            (static_cast<std::uint32_t>(raw[3]) << 24);
                base = static_cast<long>(units) * 0x200;
            }
        }
    }
    char product[17]{};
    if (base >= 0 && std::fseek(file, base + 0x150, SEEK_SET) == 0 &&
        std::fread(product, 1, 16, file) == 16) {
        // "CTR-P-AQEE": the game's code is what follows the last dash.
        const std::string text{product};
        const std::size_t dash = text.rfind('-');
        if (dash != std::string::npos && text.size() - dash - 1 == 4) {
            code = text.substr(dash + 1);
            const bool plain = std::all_of(code.begin(), code.end(), [](char ch) {
                return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
            });
            if (!plain) {
                code.clear();
            }
        }
    }
    std::fclose(file);
    return code;
}

std::shared_ptr<const CoverArt> GetCover(const std::string& game_code, std::uint64_t program_id,
                                         int& budget) {
    if (s_seen_generation != s_generation.load()) {
        // New files arrived: forget the "nothing here" answers.
        s_seen_generation = s_generation.load();
        if (s_flush_all.exchange(false)) {
            s_cache.clear(); // Files were replaced: what is loaded may be the old picture.
        }
        for (auto it = s_cache.begin(); it != s_cache.end();) {
            it = it->second.art ? std::next(it) : s_cache.erase(it);
        }
    }
    const std::string key = !game_code.empty() ? game_code : TitleIdText(program_id);
    if (const auto it = s_cache.find(key); it != s_cache.end()) {
        it->second.last_used = ++s_clock;
        return it->second.art;
    }
    if (budget <= 0) {
        return nullptr;
    }
    --budget;
    bool full = false;
    std::string path = FindCoverFile(game_code, full);
    if (path.empty() && program_id != 0) {
        path = FindCoverFile(TitleIdText(program_id), full);
    }
    CacheEntry entry;
    entry.last_used = ++s_clock;
    if (!path.empty()) {
        entry.art = LoadCoverFile(path, full);
    }
    const auto art = entry.art;
    s_cache.emplace(key, std::move(entry));
    EvictIfNeeded();
    return art;
}

bool HasCoverFile(const std::string& game_code, std::uint64_t program_id) {
    bool full = false;
    if (!FindCoverFile(game_code, full).empty()) {
        return true;
    }
    return program_id != 0 && !FindCoverFile(TitleIdText(program_id), full).empty();
}

void EnsureCoverFolders() {
    const std::string dir = CoversDir();
    mkdir(dir.c_str(), 0777);
    mkdir((dir + "/full").c_str(), 0777);
    const std::string readme = dir + "/README.txt";
    if (FileExists(readme)) {
        return;
    }
    if (std::FILE* file = std::fopen(readme.c_str(), "w")) {
        std::fputs("Cover art for the library.\n\n"
                   "  full/<ID>.jpg or .png   the whole wrap: back, spine and front.\n"
                   "                          The shelf can turn these round (X).\n"
                   "  <ID>.jpg or .png        the front only.\n\n"
                   "<ID> is the game's four-character code (AQEE for CTR-P-AQEE; it is\n"
                   "shown on the back of each case in the library) or its 16-digit title ID.\n\n"
                   "With internet access the missing ones are fetched from GameTDB\n"
                   "(art.gametdb.com/3ds/coverfull/<region>/<ID>.jpg and .../cover/...).\n"
                   "Without it, tools/covers/fetch_covers.py in the source repository\n"
                   "downloads them on a PC and sends them over FTP.\n",
                   file);
        std::fclose(file);
    }
}

void ResetCoverCache() {
    s_cache.clear();
}

std::atomic<int> s_library_style{0};

int GetLibraryStyle() {
    return s_library_style.load();
}

void SetLibraryStyle(int style) {
    s_library_style.store(std::clamp(style, 0, static_cast<int>(Ui::ShelfStyle::Count) - 1));
}

std::atomic<int> s_ui_theme{0};

int GetUiTheme() {
    return s_ui_theme.load();
}

void SetUiTheme(int theme) {
    s_ui_theme.store(theme != 0 ? 1 : 0);
    Ui::ApplyTheme(theme != 0);
}

int GetCoverSource() {
    return s_source.load();
}

void SetCoverSource(int source) {
    s_source.store(std::clamp(source, 0, static_cast<int>(CoverSource::Count) - 1));
}

namespace {
/// The games GameTDB can know, once each.
std::vector<std::string> CodesOf(const std::vector<CoverRequest>& requests) {
    std::vector<std::string> codes;
    for (const CoverRequest& request : requests) {
        if (request.game_code.size() == 4 &&
            std::find(codes.begin(), codes.end(), request.game_code) == codes.end()) {
            codes.push_back(request.game_code);
        }
    }
    return codes;
}
} // namespace

int CountCoversPresent(const std::vector<CoverRequest>& requests) {
    int present = 0;
    for (const std::string& code : CodesOf(requests)) {
        bool full = false;
        present += FindCoverFile(code, full).empty() ? 0 : 1;
    }
    return present;
}

int CountCoversMissing(const std::vector<CoverRequest>& requests) {
    return static_cast<int>(CodesOf(requests).size()) - CountCoversPresent(requests);
}

int CoverStoredQuality() {
    std::FILE* file = std::fopen((CoversDir() + "/quality.txt").c_str(), "r");
    if (file == nullptr) {
        return -1;
    }
    const int ch = std::fgetc(file);
    std::fclose(file);
    return ch == '0' ? 0 : ch == '1' ? 1 : -1;
}

void StartCoverDownload(std::vector<CoverRequest> requests, bool manual, bool overwrite) {
    if (s_active.load() ||
        (!manual && s_source.load() == static_cast<int>(CoverSource::Off))) {
        return;
    }
    // Only games with a code GameTDB can know, and no cover on the card yet.
    std::vector<CoverRequest> missing;
    for (CoverRequest& request : requests) {
        bool full = false;
        if (request.game_code.size() == 4 &&
            (overwrite || FindCoverFile(request.game_code, full).empty()) &&
            std::none_of(missing.begin(), missing.end(), [&](const CoverRequest& other) {
                return other.game_code == request.game_code;
            })) {
            missing.push_back(std::move(request));
        }
    }
    if (missing.empty()) {
        if (manual) {
            SetStatus("Nothing missing", requests.empty()
                                             ? "There are no games to find covers for."
                                             : "Every game already has a cover.");
        }
        return;
    }
    StopCoverDownload();
    // On this thread, before any worker exists: libcurl's lazy start-up is not safe when
    // two threads (this download, the update check) make their first call at once.
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) {
        return;
    }
    s_cancel = false;
    s_overwrite = overwrite;
    s_done = 0;
    s_total = static_cast<int>(missing.size());
    SetStatus("0 / " + std::to_string(missing.size()), "Downloading covers");
    s_active = true;
    s_thread = std::thread{DownloadThread, std::move(missing)};
}

void StopCoverDownload() {
    s_cancel = true;
    if (s_thread.joinable()) {
        s_thread.join();
    }
    s_active = false;
}

bool IsCoverDownloadActive() {
    return s_active.load();
}

int CoverDownloadDone() {
    return s_done.load();
}

int CoverDownloadTotal() {
    return s_total.load();
}

std::string CoverDownloadStatus() {
    std::lock_guard lock{s_status_mutex};
    return s_status_brief;
}

std::string CoverDownloadDetail() {
    std::lock_guard lock{s_status_mutex};
    return s_status_detail;
}

int GetForwardView() {
    return s_forward_view.load();
}

void SetForwardView(int view) {
    s_forward_view.store(view != 0 ? 1 : 0);
}

bool IsCoversShown() {
    return s_covers_shown.load();
}

void SetCoversShown(bool shown) {
    s_covers_shown.store(shown);
}

bool IsShelfCasesOn() {
    return Ui::g_shelf_cases;
}

void SetShelfCasesOn(bool on) {
    Ui::g_shelf_cases = on;
}

std::string CoverShareFolder() {
    return CoversDir();
}

void CoverShareLog(const std::string& text) {
    LOG_INFO(Frontend, "Cover share: {}", text);
}

void CoverShareArrived() {
    ++s_generation;
}

void CoverShareFinished() {
    s_flush_all = true;
    ++s_generation;
}

int GetCoverQuality() {
    return s_quality.load();
}

void SetCoverQuality(int quality) {
    s_quality.store(quality != 0 ? 1 : 0);
}

bool IsShelfArrowsShown() {
    return s_shelf_arrows.load();
}

void SetShelfArrowsShown(bool shown) {
    s_shelf_arrows.store(shown);
}

bool IsShelfTitleShown() {
    return s_shelf_title.load();
}

void SetShelfTitleShown(bool shown) {
    s_shelf_title.store(shown);
}

bool IsShelfReflectionOn() {
    return Ui::g_shelf_reflections;
}

void SetShelfReflectionOn(bool on) {
    Ui::g_shelf_reflections = on;
}

bool IsShelfFloatingOn() {
    return Ui::g_shelf_floating;
}

void SetShelfFloatingOn(bool on) {
    Ui::g_shelf_floating = on;
}

int GetShelfBarStyle() {
    return s_shelf_bar.load();
}

void SetShelfBarStyle(int style) {
    s_shelf_bar.store(std::clamp(style, 0, 2));
}

int CoverGeneration() {
    return s_generation.load();
}

} // namespace SwitchFrontend
