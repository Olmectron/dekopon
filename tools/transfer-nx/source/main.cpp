// SPDX-License-Identifier: GPL-2.0-or-later
//
// Toronja Transfer - moves files between a PC and the console's SD card over the local
// network, as fast as the card allows.
//
// A TCP server on port 28302 with a small protocol of its own (see README.md). What makes it
// quick: large socket buffers, 2 MiB blocks, the file written at its final size up front,
// and the network and the card each on a thread of their own, so neither waits for the
// other. The checksum (CRC-32) is computed here, on what actually went to or came from the
// card.
//
// Every step of the startup goes to sdmc:/switch/toronja-transfer.log, so that a failure
// can be read from the PC instead of off the screen.

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <switch.h>
#include <zlib.h>

#include "citra_switch/ui_canvas.h"

using namespace SwitchFrontend::Ui;

namespace {

constexpr char kVersion[] = "1.0.0";
constexpr u16 kPort = 28302;
constexpr u32 kMagic = 0x31585454; // "TTX1"
constexpr std::size_t kBlockSize = 2 * 1024 * 1024;
constexpr int kBlockCount = 6;
constexpr int kSocketBuffer = 1024 * 1024;
constexpr char kLogPath[] = "sdmc:/switch/toronja-transfer.log";

enum Op : u8 {
    OpPing = 0,
    OpList = 1,
    OpPut = 2,
    OpGet = 3,
    OpDelete = 4,
    OpHash = 5,
    OpMkdir = 6,
};

// ----------------------------------------------------------------------------------- log

std::mutex g_log_mutex;
std::vector<std::string> g_lines; // The last few, for the screen.

void AddLine(const char* text, bool to_file) {
    std::lock_guard lock{g_log_mutex};
    if (to_file) {
        if (FILE* file = std::fopen(kLogPath, "a")) {
            std::fprintf(file, "%s\n", text);
            std::fclose(file);
        }
    }
    g_lines.emplace_back(text);
    if (g_lines.size() > 12) {
        g_lines.erase(g_lines.begin());
    }
}

/// To the screen and to the log file.
__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    AddLine(text, true);
}

/// To the screen only: one line per file transferred, which would otherwise cost a write
/// to the card for each of a thousand small files.
__attribute__((format(printf, 1, 2))) void Say(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    AddLine(text, false);
}

// ------------------------------------------------------------------------- shared state

std::atomic<bool> g_quit{false};
std::atomic<u64> g_current_done{0};
std::atomic<u64> g_current_total{0};
std::atomic<u64> g_total_bytes{0};
std::atomic<u32> g_total_files{0};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_connected{false};
std::mutex g_state_mutex;
std::string g_current_name;
std::string g_last_result;

void SetCurrent(const char* verb, const std::string& path, u64 total) {
    std::lock_guard lock{g_state_mutex};
    g_current_name = std::string{verb} + " " + path;
    g_current_done = 0;
    g_current_total = total;
    g_busy = true;
}

// ----------------------------------------------------------------------------- blocks

/// The blocks that travel between the network thread and the card thread.
struct Block {
    u8* data{};
    std::size_t size{};
};

class Pipe {
public:
    bool Init() {
        for (int i = 0; i < kBlockCount; ++i) {
            u8* memory = static_cast<u8*>(std::aligned_alloc(0x1000, kBlockSize));
            if (memory == nullptr) {
                return false;
            }
            storage.push_back(memory);
        }
        return true;
    }

    void Reset() {
        std::lock_guard lock{mutex};
        free_blocks = storage;
        filled.clear();
        closed = false;
        aborted = false;
    }

    /// An empty block to fill. Null once the other side has given up.
    u8* Acquire() {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return !free_blocks.empty() || aborted; });
        if (aborted) {
            return nullptr;
        }
        u8* block = free_blocks.back();
        free_blocks.pop_back();
        return block;
    }

    void Push(u8* data, std::size_t size) {
        {
            std::lock_guard lock{mutex};
            filled.push_back({data, size});
        }
        cv.notify_all();
    }

    /// No more blocks will be pushed.
    void Close() {
        {
            std::lock_guard lock{mutex};
            closed = true;
        }
        cv.notify_all();
    }

    /// Either side failed: wake everybody up.
    void Abort() {
        {
            std::lock_guard lock{mutex};
            aborted = true;
        }
        cv.notify_all();
    }

    bool Aborted() {
        std::lock_guard lock{mutex};
        return aborted;
    }

    /// The next filled block. False at the end, or when aborted.
    bool Pop(Block& out) {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return !filled.empty() || closed || aborted; });
        if (aborted || filled.empty()) {
            return false;
        }
        out = filled.front();
        filled.erase(filled.begin());
        return true;
    }

    void Release(u8* data) {
        {
            std::lock_guard lock{mutex};
            free_blocks.push_back(data);
        }
        cv.notify_all();
    }

    u8* Scratch() {
        return storage[0];
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<u8*> storage;
    std::vector<u8*> free_blocks;
    std::vector<Block> filled;
    bool closed{};
    bool aborted{};
};

Pipe g_pipe;

// ---------------------------------------------------------------------------- sockets

bool SendAll(int fd, const void* data, std::size_t size) {
    const u8* p = static_cast<const u8*>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, p, size, 0);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool RecvAll(int fd, void* data, std::size_t size) {
    u8* p = static_cast<u8*>(data);
    while (size > 0) {
        const ssize_t got = recv(fd, p, size, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

void TuneSocket(int fd) {
    int size = kSocketBuffer;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

void SetTimeouts(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/// A reply: status 0 and nothing else, or a status and the reason as text.
bool SendStatus(int fd, int error, const char* what = "") {
    if (error == 0) {
        const u8 ok = 0;
        return SendAll(fd, &ok, 1);
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%s%s%s", what, *what ? ": " : "", std::strerror(error));
    const u16 length = static_cast<u16>(std::strlen(text));
    u8 head[3] = {1, static_cast<u8>(length), static_cast<u8>(length >> 8)};
    return SendAll(fd, head, 3) && SendAll(fd, text, length);
}

bool SendError(int fd, const char* text) {
    const u16 length = static_cast<u16>(std::strlen(text));
    u8 head[3] = {1, static_cast<u8>(length), static_cast<u8>(length >> 8)};
    return SendAll(fd, head, 3) && SendAll(fd, text, length);
}

// ------------------------------------------------------------------------------ files

/// "/switch/a.nro" as the client names it -> "sdmc:/switch/a.nro". Empty when not allowed.
std::string CardPath(const std::string& path) {
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return {};
    }
    std::string out = "sdmc:" + path;
    while (out.size() > 6 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

/// Creates every folder on the way to `path` (the last component too when `whole`).
void MakeFolders(const std::string& path, bool whole) {
    for (std::size_t i = 6; i < path.size(); ++i) { // Past "sdmc:/".
        if (path[i] == '/') {
            mkdir(path.substr(0, i).c_str(), 0777);
        }
    }
    if (whole) {
        mkdir(path.c_str(), 0777);
    }
}

bool WriteAll(int file, const u8* data, std::size_t size) {
    while (size > 0) {
        const ssize_t done = write(file, data, size);
        if (done <= 0) {
            return false;
        }
        data += done;
        size -= static_cast<std::size_t>(done);
    }
    return true;
}

// -------------------------------------------------------------------------- commands

bool DoPing(int fd) {
    u64 free_bytes = 0;
    u64 total_bytes = 0;
    struct statvfs info {};
    if (statvfs("sdmc:/", &info) == 0) {
        free_bytes = static_cast<u64>(info.f_bavail) * info.f_frsize;
        total_bytes = static_cast<u64>(info.f_blocks) * info.f_frsize;
    }
    char text[128];
#if defined(__SWITCH2__)
    const char* console = "switch2";
#else
    const char* console = "switch1";
#endif
    const int length = std::snprintf(text, sizeof(text), "Toronja Transfer %s %s", kVersion,
                                     console);
    const u16 size = static_cast<u16>(length);
    return SendStatus(fd, 0) && SendAll(fd, &free_bytes, 8) && SendAll(fd, &total_bytes, 8) &&
           SendAll(fd, &size, 2) && SendAll(fd, text, size);
}

bool DoList(int fd, const std::string& path) {
    const std::string dir_path = path == "sdmc:" ? "sdmc:/" : path;
    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        return SendStatus(fd, errno, "opendir");
    }
    std::vector<u8> out;
    u32 count = 0;
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        u8 type = entry->d_type == DT_DIR ? 1 : 0;
        u64 size = 0;
        u64 mtime = 0;
        struct stat info {};
        const std::string full = (dir_path.back() == '/' ? dir_path : dir_path + "/") + name;
        if (stat(full.c_str(), &info) == 0) {
            type = S_ISDIR(info.st_mode) ? 1 : 0;
            size = static_cast<u64>(info.st_size);
            mtime = static_cast<u64>(info.st_mtime);
        }
        const u16 length = static_cast<u16>(name.size());
        out.push_back(type);
        out.insert(out.end(), reinterpret_cast<u8*>(&size), reinterpret_cast<u8*>(&size) + 8);
        out.insert(out.end(), reinterpret_cast<u8*>(&mtime), reinterpret_cast<u8*>(&mtime) + 8);
        out.insert(out.end(), reinterpret_cast<const u8*>(&length),
                   reinterpret_cast<const u8*>(&length) + 2);
        out.insert(out.end(), name.begin(), name.begin() + length);
        ++count;
    }
    closedir(dir);
    const u32 bytes = static_cast<u32>(out.size());
    return SendStatus(fd, 0) && SendAll(fd, &count, 4) && SendAll(fd, &bytes, 4) &&
           SendAll(fd, out.data(), out.size());
}

/// PC -> card. The network fills blocks on this thread; another thread writes them.
bool DoPut(int fd, const std::string& path, const std::string& shown) {
    u64 size = 0;
    if (!RecvAll(fd, &size, 8)) {
        return false;
    }
    MakeFolders(path, false);
    const std::string part = path + ".ttpart";
    const int file = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0) {
        SendStatus(fd, errno, "create");
        return false; // The data that follows cannot be told from a command: hang up.
    }
    // The whole size at once: growing the file block by block is what makes FAT slow.
    if (size > 0 && ftruncate(file, static_cast<off_t>(size)) != 0) {
        const int error = errno;
        close(file);
        unlink(part.c_str());
        SendStatus(fd, error, "reserve space");
        return false;
    }
    if (!SendStatus(fd, 0)) {
        close(file);
        unlink(part.c_str());
        return false;
    }

    SetCurrent("Receiving", shown, size);
    const u64 started = armGetSystemTick();
    g_pipe.Reset();
    u32 crc = crc32(0, nullptr, 0);
    int write_error = 0;
    std::thread writer{[&] {
        Block block;
        while (g_pipe.Pop(block)) {
            crc = crc32(crc, block.data, static_cast<uInt>(block.size));
            if (!WriteAll(file, block.data, block.size)) {
                write_error = errno != 0 ? errno : EIO;
                g_pipe.Abort();
                return;
            }
            g_current_done += block.size;
            g_pipe.Release(block.data);
        }
    }};

    bool network_ok = true;
    u64 left = size;
    while (left > 0) {
        u8* block = g_pipe.Acquire();
        if (block == nullptr) {
            break;
        }
        const std::size_t want = static_cast<std::size_t>(std::min<u64>(left, kBlockSize));
        if (!RecvAll(fd, block, want)) {
            network_ok = false;
            g_pipe.Abort();
            break;
        }
        g_pipe.Push(block, want);
        left -= want;
    }
    g_pipe.Close();
    writer.join();
    const bool failed = !network_ok || write_error != 0 || g_pipe.Aborted();
    if (close(file) != 0 && write_error == 0 && !failed) {
        write_error = errno != 0 ? errno : EIO;
    }
    g_busy = false;

    if (failed || write_error != 0) {
        unlink(part.c_str());
        if (write_error != 0) {
            Log("PUT %s: write failed (%s)", shown.c_str(), std::strerror(write_error));
            SendStatus(fd, write_error, "write");
        } else {
            Log("PUT %s: connection lost", shown.c_str());
        }
        return false;
    }
    unlink(path.c_str());
    if (rename(part.c_str(), path.c_str()) != 0) {
        const int error = errno;
        unlink(part.c_str());
        Log("PUT %s: rename failed (%s)", shown.c_str(), std::strerror(error));
        return SendStatus(fd, error, "rename");
    }
    const double seconds = armTicksToNs(armGetSystemTick() - started) / 1e9;
    const double mbps = seconds > 0 ? size / 1048576.0 / seconds : 0;
    Say("PUT %s: %llu bytes, %.1f MB/s", shown.c_str(), static_cast<unsigned long long>(size),
        mbps);
    g_total_bytes += size;
    ++g_total_files;
    return SendStatus(fd, 0) && SendAll(fd, &crc, 4);
}

/// Card -> PC. A thread reads the card into blocks; this one sends them.
bool DoGet(int fd, const std::string& path, const std::string& shown, bool send_data) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return SendStatus(fd, errno, "stat");
    }
    if (S_ISDIR(info.st_mode)) {
        return SendError(fd, "is a folder");
    }
    const int file = open(path.c_str(), O_RDONLY);
    if (file < 0) {
        return SendStatus(fd, errno, "open");
    }
    const u64 size = static_cast<u64>(info.st_size);
    if (!SendStatus(fd, 0) || !SendAll(fd, &size, 8)) {
        close(file);
        return false;
    }

    SetCurrent(send_data ? "Sending" : "Checking", shown, size);
    const u64 started = armGetSystemTick();
    g_pipe.Reset();
    u32 crc = crc32(0, nullptr, 0);
    bool read_ok = true;
    std::thread reader{[&] {
        u64 left = size;
        while (left > 0) {
            u8* block = g_pipe.Acquire();
            if (block == nullptr) {
                return;
            }
            const std::size_t want = static_cast<std::size_t>(std::min<u64>(left, kBlockSize));
            std::size_t have = 0;
            while (have < want) {
                const ssize_t got = read(file, block + have, want - have);
                if (got <= 0) {
                    read_ok = false;
                    g_pipe.Abort();
                    return;
                }
                have += static_cast<std::size_t>(got);
            }
            crc = crc32(crc, block, static_cast<uInt>(want));
            g_pipe.Push(block, want);
            left -= want;
        }
        g_pipe.Close();
    }};

    bool network_ok = true;
    Block block;
    while (g_pipe.Pop(block)) {
        if (send_data && !SendAll(fd, block.data, block.size)) {
            network_ok = false;
            g_pipe.Abort();
            break;
        }
        g_current_done += block.size;
        g_pipe.Release(block.data);
    }
    reader.join();
    close(file);
    g_busy = false;
    if (!network_ok || !read_ok) {
        Log("%s %s: %s", send_data ? "GET" : "HASH", shown.c_str(),
            read_ok ? "connection lost" : "read failed");
        return false; // The client was promised `size` bytes: all that is left is to hang up.
    }
    if (send_data) {
        const double seconds = armTicksToNs(armGetSystemTick() - started) / 1e9;
        const double mbps = seconds > 0 ? size / 1048576.0 / seconds : 0;
        Say("GET %s: %llu bytes, %.1f MB/s", shown.c_str(),
            static_cast<unsigned long long>(size), mbps);
        g_total_bytes += size;
        ++g_total_files;
    }
    return SendAll(fd, &crc, 4);
}

bool DoDelete(int fd, const std::string& path, const std::string& shown) {
    if (path == "sdmc:") {
        return SendError(fd, "not the whole card");
    }
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return SendStatus(fd, errno, "stat");
    }
    const int result = S_ISDIR(info.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str());
    if (result != 0) {
        return SendStatus(fd, errno, S_ISDIR(info.st_mode) ? "rmdir" : "delete");
    }
    Say("DELETE %s", shown.c_str());
    return SendStatus(fd, 0);
}

/// One client, command after command, until it hangs up.
void Serve(int fd) {
    for (;;) {
        // Idle between commands: wait without a deadline, but notice the app closing.
        pollfd waiting{fd, POLLIN, 0};
        const int ready = poll(&waiting, 1, 200);
        if (g_quit) {
            return;
        }
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            return;
        }
        u8 head[7];
        if (!RecvAll(fd, head, sizeof(head))) {
            return;
        }
        u32 magic;
        std::memcpy(&magic, head, 4);
        const u8 op = head[4];
        const u16 path_length = static_cast<u16>(head[5] | (head[6] << 8));
        if (magic != kMagic || path_length > 1024) {
            Log("Bad request, hanging up");
            return;
        }
        std::string shown(path_length, '\0');
        if (path_length > 0 && !RecvAll(fd, shown.data(), path_length)) {
            return;
        }
        if (op == OpPing) {
            if (!DoPing(fd)) {
                return;
            }
            continue;
        }
        const std::string path = CardPath(shown);
        if (path.empty()) {
            SendError(fd, "bad path");
            return; // A PUT's data may follow: hang up rather than read it as commands.
        }
        bool ok = false;
        switch (op) {
        case OpList:
            ok = DoList(fd, path);
            break;
        case OpPut:
            ok = DoPut(fd, path, shown);
            break;
        case OpGet:
            ok = DoGet(fd, path, shown, true);
            break;
        case OpHash:
            ok = DoGet(fd, path, shown, false);
            break;
        case OpDelete:
            ok = DoDelete(fd, path, shown);
            break;
        case OpMkdir:
            MakeFolders(path, true);
            ok = SendStatus(fd, 0);
            break;
        default:
            SendError(fd, "unknown command");
            break;
        }
        g_busy = false;
        if (!ok) {
            return;
        }
    }
}

void ServerThread() {
    int listener = -1;
    while (!g_quit) {
        if (listener < 0) {
            listener = socket(AF_INET, SOCK_STREAM, 0);
            if (listener < 0) {
                Log("socket() failed: %s", std::strerror(errno));
                svcSleepThread(1'000'000'000ULL);
                continue;
            }
            int yes = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            TuneSocket(listener); // Accepted sockets inherit the buffer sizes.
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(kPort);
            addr.sin_addr.s_addr = INADDR_ANY;
            if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                listen(listener, 2) != 0) {
                Log("bind/listen failed: %s", std::strerror(errno));
                close(listener);
                listener = -1;
                svcSleepThread(1'000'000'000ULL);
                continue;
            }
            Log("Listening on port %u", kPort);
        }
        pollfd waiting{listener, POLLIN, 0};
        const int ready = poll(&waiting, 1, 200);
        if (ready < 0 || (ready > 0 && (waiting.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
            // The network went away under us (cable pulled, sleep): start over.
            close(listener);
            listener = -1;
            continue;
        }
        if (ready == 0) {
            continue;
        }
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int fd = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (fd < 0) {
            continue;
        }
        TuneSocket(fd);
        SetTimeouts(fd, 30);
        int rcv = 0;
        int snd = 0;
        socklen_t length = sizeof(rcv);
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, &length);
        length = sizeof(snd);
        getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, &length);
        Log("Client %s connected (rcvbuf %d, sndbuf %d)", inet_ntoa(peer.sin_addr), rcv, snd);
        g_connected = true;
        Serve(fd);
        g_connected = false;
        g_busy = false;
        close(fd);
        Log("Client disconnected");
    }
    if (listener >= 0) {
        close(listener);
    }
}

// ------------------------------------------------------------------------------ screen

std::string HumanSize(u64 bytes) {
    char text[32];
    if (bytes >= 1024ULL * 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.2f GB", bytes / 1073741824.0);
    } else if (bytes >= 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
    } else {
        std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    }
    return text;
}

void Draw(Canvas& canvas, Font& font, bool font_ready, double mbps) {
    canvas.Clear(kColBg);
    if (!font_ready) {
        // No system font: a bar still says that the app is alive, and how far along it is.
        canvas.FillRect(80, 340, 1120, 40, kColSurface);
        const u64 total = g_current_total;
        if (g_busy && total > 0) {
            canvas.FillRect(80, 340, static_cast<int>(1120 * g_current_done / total), 40,
                            kColAccent);
        }
        return;
    }
    canvas.FillRect(0, 0, kPanelW, 96, kColRail);
    font.Draw(canvas, 48, 62, "Toronja Transfer", 36, kColText);
    font.Draw(canvas, 370, 62, kVersion, 22, kColTextDim);

    char text[256];
    const u32 host = gethostid();
    if (host == 0 || host == htonl(INADDR_LOOPBACK)) {
        font.Draw(canvas, 48, 160, "No network. Connect the cable or Wi-Fi.", 28, kColError);
    } else {
        in_addr address{};
        address.s_addr = host;
        std::snprintf(text, sizeof(text), "%s : %u", inet_ntoa(address), kPort);
        font.Draw(canvas, 48, 160, text, 40, kColAccent);
        font.Draw(canvas, 48, 200,
                  g_connected ? "PC connected" : "Waiting for the PC  (python transferir.py ...)",
                  22, kColTextDim);
    }

    canvas.FillRoundRect(48, 232, 1184, 150, 12, kColSurface);
    if (g_busy) {
        std::string name;
        {
            std::lock_guard lock{g_state_mutex};
            name = g_current_name;
        }
        if (name.size() > 90) {
            name = name.substr(0, 40) + "..." + name.substr(name.size() - 47);
        }
        font.Draw(canvas, 72, 276, name, 22, kColText);
        const u64 done = g_current_done;
        const u64 total = g_current_total;
        canvas.FillRoundRect(72, 296, 1136, 20, 10, kColBadge);
        if (total > 0) {
            const int width = static_cast<int>(1136.0 * std::min(done, total) / total);
            if (width > 20) {
                canvas.FillRoundRect(72, 296, width, 20, 10, kColAccent);
            }
        }
        std::snprintf(text, sizeof(text), "%s / %s    %.1f MB/s", HumanSize(done).c_str(),
                      HumanSize(total).c_str(), mbps);
        font.Draw(canvas, 72, 356, text, 24, kColText);
    } else {
        std::snprintf(text, sizeof(text), "Idle.  %u files, %s this session",
                      g_total_files.load(), HumanSize(g_total_bytes).c_str());
        font.Draw(canvas, 72, 316, text, 24, kColTextDim);
    }

    std::vector<std::string> lines;
    {
        std::lock_guard lock{g_log_mutex};
        lines = g_lines;
    }
    int y = 420;
    for (std::size_t i = lines.size() > 9 ? lines.size() - 9 : 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (line.size() > 110) {
            line.resize(110);
        }
        font.Draw(canvas, 48, y, line, 18, kColTextDim);
        y += 26;
    }
    canvas.FillRect(0, 668, kPanelW, 52, kColHintBar);
    font.Draw(canvas, 48, 702, "+  Exit        Keep this screen open while transferring", 20,
              kColTextDim);
}

/// Sockets with room for a megabyte in flight each way; the stock ones if that is refused.
void StartSockets() {
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.tcp_tx_buf_size = kSocketBuffer;
    config.tcp_rx_buf_size = kSocketBuffer;
    config.tcp_tx_buf_max_size = 2 * kSocketBuffer;
    config.tcp_rx_buf_max_size = 2 * kSocketBuffer;
    config.sb_efficiency = 4;
    Result result = socketInitialize(&config);
    if (R_SUCCEEDED(result)) {
        Log("Sockets: large buffers");
        return;
    }
    Log("Sockets: large buffers refused (0x%x), trying the defaults", result);
    result = socketInitializeDefault();
    Log("Sockets: defaults -> 0x%x", result);
}

} // namespace

int main(int, char**) {
    unlink(kLogPath);
    Log("Toronja Transfer %s starting", kVersion);
    StartSockets();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    // A transfer of several gigabytes outlasts the screen timeout.
    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);

    Canvas canvas;
    Font font;
    bool font_ready = false;
    ApplyTheme(false);
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) && font.InitLibrary()) {
        for (const PlSharedFontType type :
             {PlSharedFontType_Standard, PlSharedFontType_NintendoExt}) {
            PlFontData data{};
            if (R_SUCCEEDED(plGetSharedFontByType(&data, type))) {
                font.AddMemoryFace(data.address, data.size);
            }
        }
        font_ready = font.Valid();
    }
    Log("Font: %s", font_ready ? "ok" : "not available");

    Framebuffer fb{};
    const Result fb_result = framebufferCreate(&fb, nwindowGetDefault(), kPanelW, kPanelH,
                                               PIXEL_FORMAT_RGBA_8888, 2);
    const bool have_fb = R_SUCCEEDED(fb_result);
    if (have_fb) {
        framebufferMakeLinear(&fb);
    }
    Log("Framebuffer: 0x%x", fb_result);

    if (!g_pipe.Init()) {
        Log("Out of memory for the transfer blocks");
    }
    in_addr address{};
    address.s_addr = gethostid();
    Log("Address: %s", inet_ntoa(address));

    std::thread server{ServerThread};

    u64 last_done = 0;
    u64 last_tick = armGetSystemTick();
    double mbps = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        const u64 now = armGetSystemTick();
        const double elapsed = armTicksToNs(now - last_tick) / 1e9;
        if (elapsed >= 0.5) {
            const u64 done = g_current_done;
            mbps = done >= last_done ? (done - last_done) / 1048576.0 / elapsed : 0;
            last_done = done;
            last_tick = now;
        }
        if (have_fb) {
            Draw(canvas, font, font_ready, mbps);
            u32 stride = 0;
            u8* pixels = static_cast<u8*>(framebufferBegin(&fb, &stride));
            for (int y = 0; y < kPanelH; ++y) {
                std::memcpy(pixels + static_cast<std::size_t>(y) * stride,
                            canvas.Data() + static_cast<std::size_t>(y) * kPanelW,
                            kPanelW * sizeof(u32));
            }
            framebufferEnd(&fb);
        } else {
            svcSleepThread(16'000'000ULL);
        }
        // The picture is cheap to make, but the cores are better spent on the transfer.
        svcSleepThread(g_busy ? 80'000'000ULL : 30'000'000ULL);
    }

    g_quit = true;
    g_pipe.Abort();
    server.join();
    Log("Closing");
    if (have_fb) {
        framebufferClose(&fb);
    }
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    plExit();
    socketExit();
    return 0;
}
