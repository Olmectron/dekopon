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

/// Fetches `url` into `path`. The file only appears once the download is complete.
bool Download(const std::string& url, const std::string& path) {
    const std::string partial = path + ".part";
    std::FILE* file = std::fopen(partial.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    CURL* curl = curl_easy_init();
    bool ok = false;
    if (curl != nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "covers/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, AbortWhenCancelled);
        ok = curl_easy_perform(curl) == CURLE_OK;
        curl_easy_cleanup(curl);
    }
    const long size = std::ftell(file);
    std::fclose(file);
    if (ok && size > 1024) {
        std::remove(path.c_str());
        return std::rename(partial.c_str(), path.c_str()) == 0;
    }
    std::remove(partial.c_str());
    return false;
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

void DownloadThread(std::vector<CoverRequest> requests) {
    const std::string dir = CoversDir();
    mkdir(dir.c_str(), 0777);
    mkdir((dir + "/full").c_str(), 0777);
    bool reachable = true;
    for (const CoverRequest& request : requests) {
        if (s_cancel.load() || !reachable) {
            break;
        }
        const std::string& code = request.game_code;
        bool got = false;
        // The whole wrap if GameTDB has it, the front otherwise.
        for (const bool wrap : {true, false}) {
            for (const char* region : RegionsFor(code)) {
                if (got || s_cancel.load()) {
                    break;
                }
                const std::string url = std::string{"https://art.gametdb.com/3ds/"} +
                                        (wrap ? "coverfull" : "cover") + "/" + region + "/" + code +
                                        ".jpg";
                const std::string path = dir + (wrap ? "/full/" : "/") + code + ".jpg";
                got = Download(url, path);
            }
            if (got) {
                break;
            }
        }
        if (got) {
            ++s_generation;
        } else if (s_done.load() == 0 && &request == &requests.front()) {
            // Nothing at all for the first game: check that the site can be reached before
            // working through the rest of the library with a timeout each.
            CURL* probe = curl_easy_init();
            if (probe != nullptr) {
                curl_easy_setopt(probe, CURLOPT_URL, "https://art.gametdb.com/");
                curl_easy_setopt(probe, CURLOPT_NOBODY, 1L);
                curl_easy_setopt(probe, CURLOPT_CONNECTTIMEOUT, 5L);
                curl_easy_setopt(probe, CURLOPT_TIMEOUT, 8L);
                curl_easy_setopt(probe, CURLOPT_NOSIGNAL, 1L);
                reachable = curl_easy_perform(probe) == CURLE_OK;
                curl_easy_cleanup(probe);
            }
            if (!reachable) {
                LOG_INFO(Frontend, "Covers: the cover site cannot be reached, not downloading");
            }
        }
        ++s_done;
    }
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

int GetCoverSource() {
    return s_source.load();
}

void SetCoverSource(int source) {
    s_source.store(std::clamp(source, 0, static_cast<int>(CoverSource::Count) - 1));
}

void StartCoverDownload(std::vector<CoverRequest> requests) {
    if (s_source.load() == static_cast<int>(CoverSource::Off) || s_active.load()) {
        return;
    }
    // Only games with a code GameTDB can know, and no cover on the card yet.
    std::vector<CoverRequest> missing;
    for (CoverRequest& request : requests) {
        bool full = false;
        if (request.game_code.size() == 4 && FindCoverFile(request.game_code, full).empty() &&
            std::none_of(missing.begin(), missing.end(), [&](const CoverRequest& other) {
                return other.game_code == request.game_code;
            })) {
            missing.push_back(std::move(request));
        }
    }
    if (missing.empty()) {
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
    s_done = 0;
    s_total = static_cast<int>(missing.size());
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

int CoverGeneration() {
    return s_generation.load();
}

} // namespace SwitchFrontend
