// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Kept apart from the emulator's headers, like game_share_ldn.cpp: <switch.h> and
// core/hle/result.h do not mix.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "citra_switch/cover_share.h"
#include "citra_switch/cover_share_hooks.h"

namespace SwitchFrontend::CoverShare {
namespace {

constexpr u16 PORT = 28301;
constexpr char HELLO[] = "TCOV1";   // Opens the TCP stream.
constexpr char SEEK[] = "TCOV?";    // Broadcast by the sender.
constexpr char HERE[] = "TCOV!";    // The receiver's answer.
constexpr u32 MAX_FILE = 6u << 20;  // No cover is anywhere near this.
constexpr std::size_t MAX_NAME = 40;
constexpr u8 KIND_FRONT = 0, KIND_WRAP = 1, KIND_END = 0xFF;

// Both ends must use the same values. Not GameShare's: a console waiting for covers must
// not be mistaken for one sharing a game.
constexpr u16 LDN_SCENE_ID = 0x4356; // "CV"
constexpr u8 LDN_PASSPHRASE[0x10] = {0x54, 0x6f, 0x72, 0x6f, 0x6e, 0x6a, 0x61, 0x43,
                                     0x6f, 0x76, 0x65, 0x72, 0x73, 0x30, 0x30, 0x31};

std::thread s_thread;
std::atomic<bool> s_active{false};
std::atomic<bool> s_cancel{false};
std::atomic<bool> s_sending{false};
std::atomic<bool> s_failed{false};
std::atomic<int> s_serial{0};
std::mutex s_mutex;
std::string s_brief;
std::string s_detail;

void Log(const std::string& text) {
    CoverShareLog(text);
}

/// What the row shows; `announce` also tells the user (a notice), for steps and endings.
void Status(std::string brief, std::string detail, bool announce, bool failed = false) {
    Log(detail);
    std::lock_guard lock{s_mutex};
    s_brief = std::move(brief);
    s_detail = std::move(detail);
    if (announce) {
        s_failed = failed;
        ++s_serial;
    }
}

void Fail(std::string detail) {
    Status("Failed", std::move(detail), true, true);
}

std::string Hex(Result rc) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%x", rc);
    return buffer;
}

bool Cancelled() {
    return s_cancel.load();
}

// ------------------------------------------------------------------------------- sockets

void CloseSocket(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void SetTimeouts(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/// Everything or failure. Gives up when cancelled or when the other end goes quiet.
bool SendAll(int fd, const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        if (Cancelled()) {
            return false;
        }
        const ssize_t n = send(fd, p, size, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool RecvAll(int fd, void* data, std::size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        if (Cancelled()) {
            return false;
        }
        const ssize_t n = recv(fd, p, size, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool SendU32(int fd, u32 value) {
    const u32 wire = htonl(value);
    return SendAll(fd, &wire, sizeof(wire));
}

bool RecvU32(int fd, u32& value) {
    u32 wire = 0;
    if (!RecvAll(fd, &wire, sizeof(wire))) {
        return false;
    }
    value = ntohl(wire);
    return true;
}

/// FNV-1a: enough to notice a file that arrived damaged.
u32 Checksum(const std::vector<u8>& data) {
    u32 hash = 2166136261u;
    for (const u8 byte : data) {
        hash = (hash ^ byte) * 16777619u;
    }
    return hash;
}

int OpenUdp(bool bind_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind_port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            CloseSocket(fd);
        }
    }
    return fd;
}

int OpenListener() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        CloseSocket(fd);
    }
    return fd;
}

/// Connects with a time limit (a plain connect can sit for minutes on a dead address).
int ConnectTo(in_addr address, int seconds) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr = address;
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        CloseSocket(fd);
        return -1;
    }
    for (int waited = 0; rc != 0 && waited < seconds * 10; ++waited) {
        if (Cancelled()) {
            CloseSocket(fd);
            return -1;
        }
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, 100) > 0) {
            int error = 0;
            socklen_t length = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
            if (error != 0 || (p.revents & (POLLERR | POLLHUP)) != 0) {
                CloseSocket(fd);
                return -1;
            }
            rc = 0;
        }
    }
    if (rc != 0) {
        CloseSocket(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    SetTimeouts(fd, 15);
    return fd;
}

// --------------------------------------------------------------------------------- files

struct CoverFile {
    std::string name;
    std::string path;
    u8 kind{};
    u32 size{};
};

bool GoodName(const std::string& name) {
    if (name.empty() || name.size() > MAX_NAME || name.front() == '.') {
        return false;
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") {
        return false;
    }
    // Letters, digits and the one dot: nothing that could name another folder.
    return std::all_of(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(dot),
                       [](unsigned char ch) { return std::isalnum(ch) != 0; });
}

void ListCovers(const std::string& dir, u8 kind, std::vector<CoverFile>& out) {
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return;
    }
    while (const dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (!GoodName(name)) {
            continue;
        }
        const std::string path = dir + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            static_cast<u64>(st.st_size) > MAX_FILE) {
            continue;
        }
        out.push_back({name, path, kind, static_cast<u32>(st.st_size)});
    }
    closedir(handle);
}

bool ReadFile(const std::string& path, std::vector<u8>& data) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t got = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    return got == data.size();
}

bool WriteFileSafely(const std::string& path, const std::vector<u8>& data) {
    const std::string partial = path + ".part";
    std::FILE* file = std::fopen(partial.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t put = std::fwrite(data.data(), 1, data.size(), file);
    const bool closed = std::fclose(file) == 0;
    if (put != data.size() || !closed) {
        std::remove(partial.c_str());
        return false;
    }
    std::remove(path.c_str());
    if (std::rename(partial.c_str(), path.c_str()) != 0) {
        std::remove(partial.c_str());
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------------- LDN

bool s_ldn_open = false;

void CloseLdn() {
    if (s_ldn_open) {
        ldnExit();
        s_ldn_open = false;
    }
}

LdnSecurityConfig Security() {
    LdnSecurityConfig security{};
    security.security_mode = LdnSecurityMode_Product;
    security.passphrase_size = sizeof(LDN_PASSPHRASE);
    std::memcpy(security.passphrase, LDN_PASSPHRASE, sizeof(LDN_PASSPHRASE));
    return security;
}

/// The receiver's side: a network of its own for the sender to join.
bool HostDirectWireless(std::string& why) {
    Result rc = ldnInitialize(LdnServiceType_User);
    if (R_FAILED(rc)) {
        why = "ldnInitialize " + Hex(rc);
        return false;
    }
    s_ldn_open = true;
    ldnSetProtocol(LdnProtocol_Unknown3); // Mixed Switch 1 / Switch 2; harmless if refused.
    rc = ldnOpenAccessPoint();
    if (R_FAILED(rc)) {
        why = "ldnOpenAccessPoint " + Hex(rc);
        CloseLdn();
        return false;
    }
    LdnSecurityConfig security = Security();
    LdnUserConfig user{};
    std::strncpy(user.user_name, "ToronjaCovers", sizeof(user.user_name) - 1);
    LdnNetworkConfig network{};
    network.intent_id.local_communication_id = -1;
    network.intent_id.scene_id = LDN_SCENE_ID;
    network.channel = 0;
    network.node_count_max = 2;
    network.local_communication_version = 0;
    rc = ldnCreateNetwork(&security, &user, &network);
    if (R_FAILED(rc)) {
        why = "ldnCreateNetwork " + Hex(rc);
        CloseLdn();
        return false;
    }
    return true;
}

/// The sender's side: finds the receiver's network and joins it. `host` is the receiver.
bool JoinDirectWireless(in_addr& host, std::string& why, int seconds) {
    static LdnNetworkInfo networks[0x18];
    const u64 deadline = armGetSystemTick() + armNsToTicks(static_cast<u64>(seconds) * 1'000'000'000ull);
    Result last = 0;
    bool refused_by_title = false;
    // The system flavour of the service first: the user one only lets a title join networks
    // listed in its own NACP, and the other console may be running as a different title.
    for (int attempt = 0; armGetSystemTick() < deadline && !Cancelled(); ++attempt) {
        const LdnServiceType service = (attempt / 2) % 2 == 0 ? LdnServiceType_System
                                                             : LdnServiceType_User;
        Result rc = ldnInitialize(service);
        if (R_FAILED(rc)) {
            last = rc;
            svcSleepThread(200'000'000);
            continue;
        }
        s_ldn_open = true;
        ldnSetProtocol(attempt % 2 == 0 ? LdnProtocol_Unknown3 : LdnProtocol_NX);
        rc = ldnOpenStation();
        if (R_FAILED(rc)) {
            last = rc;
            CloseLdn();
            svcSleepThread(200'000'000);
            continue;
        }
        for (int scan = 0; scan < 5 && armGetSystemTick() < deadline && !Cancelled(); ++scan) {
            LdnScanFilter filter{};
            filter.network_id.intent_id.local_communication_id = -1;
            filter.network_id.intent_id.scene_id = LDN_SCENE_ID;
            filter.flags = LdnScanFilterFlag_SceneId;
            s32 total = 0;
            rc = ldnScan(0, &filter, networks, 0x18, &total);
            if (R_FAILED(rc) || total <= 0) {
                if (R_FAILED(rc)) {
                    last = rc;
                }
                svcSleepThread(300'000'000);
                continue;
            }
            LdnSecurityConfig security = Security();
            LdnUserConfig user{};
            std::strncpy(user.user_name, "ToronjaCovers", sizeof(user.user_name) - 1);
            rc = ldnConnect(&security, &user, 0, 0, &networks[0]);
            Log("Cover share: ldnConnect " + Hex(rc));
            if (R_FAILED(rc)) {
                last = rc;
                refused_by_title = refused_by_title || rc == 0xc2cb;
                break; // Another flavour of the service, another protocol.
            }
            LdnNetworkInfo info{};
            rc = ldnGetNetworkInfo(&info);
            if (R_FAILED(rc) || info.node_count == 0) {
                last = rc;
                break;
            }
            // The first node is the access point: the receiver.
            host.s_addr = htonl(info.nodes[0].ip_addr.addr);
            return true;
        }
        CloseLdn();
    }
    if (Cancelled()) {
        why = "stopped";
    } else if (refused_by_title) {
        why = "the system refused the link (0xc2cb): this happens when Toronja runs over a "
              "game. Open the Homebrew Menu from the Album on this console, or put both "
              "consoles on the same network";
    } else if (last != 0) {
        why = "no console waiting for covers was found nearby (last answer " + Hex(last) + ")";
    } else {
        why = "no console waiting for covers was found nearby";
    }
    return false;
}

// -------------------------------------------------------------------------------- sender

bool FindOnNetwork(in_addr& host, int seconds) {
    int udp = OpenUdp(false);
    if (udp < 0) {
        return false;
    }
    bool found = false;
    for (int round = 0; round < seconds * 2 && !found && !Cancelled(); ++round) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(PORT);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(udp, SEEK, sizeof(SEEK), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        // And the broadcast address of a /24 around our own address, for routers that drop
        // the limited one.
        const u32 own = gethostid();
        if (own != 0 && own != htonl(INADDR_LOOPBACK)) {
            to.sin_addr.s_addr = (own & htonl(0xFFFFFF00u)) | htonl(0x000000FFu);
            sendto(udp, SEEK, sizeof(SEEK), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        pollfd p{udp, POLLIN, 0};
        if (poll(&p, 1, 500) > 0 && (p.revents & POLLIN) != 0) {
            char answer[16]{};
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const ssize_t n = recvfrom(udp, answer, sizeof(answer), 0,
                                       reinterpret_cast<sockaddr*>(&from), &length);
            if (n >= static_cast<ssize_t>(sizeof(HERE)) &&
                std::memcmp(answer, HERE, sizeof(HERE)) == 0) {
                host = from.sin_addr;
                found = true;
            }
        }
    }
    CloseSocket(udp);
    return found;
}

void SendThread() {
    std::vector<CoverFile> files;
    const std::string dir = CoverShareFolder();
    ListCovers(dir + "/full", KIND_WRAP, files);
    ListCovers(dir, KIND_FRONT, files);
    if (files.empty()) {
        Fail("There are no covers on this console to send.");
        s_active = false;
        return;
    }

    in_addr host{};
    Status("Searching", "Looking for the other console on the network...", true);
    bool found = FindOnNetwork(host, 4);
    if (!found && !Cancelled()) {
        Status("Searching", "Looking for the other console nearby (direct wireless)...", true);
        std::string why;
        found = JoinDirectWireless(host, why, 60);
        if (!found && !Cancelled()) {
            Fail("Covers were not sent: " + why + ".");
        }
    }
    int fd = -1;
    if (found && !Cancelled()) {
        // The receiver may still be opening its sockets on the new link.
        for (int attempt = 0; attempt < 5 && fd < 0 && !Cancelled(); ++attempt) {
            fd = ConnectTo(host, 6);
            if (fd < 0) {
                svcSleepThread(500'000'000);
            }
        }
        if (fd < 0 && !Cancelled()) {
            Fail("Covers were not sent: the other console was found but did not answer.");
        }
    }

    if (fd >= 0) {
        const u32 total = static_cast<u32>(files.size());
        u32 stored = 0, refused = 0;
        bool ok = SendAll(fd, HELLO, sizeof(HELLO)) && SendU32(fd, total);
        std::vector<u8> data;
        for (u32 i = 0; ok && i < total; ++i) {
            const CoverFile& file = files[i];
            data.resize(file.size);
            if (!ReadFile(file.path, data)) {
                ++refused; // Unreadable here: skip it, the rest can still go.
                continue;
            }
            const u8 head[2] = {file.kind, static_cast<u8>(file.name.size())};
            u8 answer = 0;
            ok = SendAll(fd, head, sizeof(head)) && SendAll(fd, file.name.data(), file.name.size()) &&
                 SendU32(fd, file.size) && SendU32(fd, Checksum(data)) &&
                 SendAll(fd, data.data(), data.size()) && RecvAll(fd, &answer, 1);
            if (ok) {
                answer == 'K' ? ++stored : ++refused;
                Status(std::to_string(i + 1) + " / " + std::to_string(total), "Sending covers",
                       false);
            }
        }
        const u8 end[2] = {KIND_END, 0};
        ok = ok && SendAll(fd, end, sizeof(end));
        CloseSocket(fd);
        if (Cancelled()) {
            Status("Stopped", "Sending covers was stopped; " + std::to_string(stored) +
                                  " had arrived.", true);
        } else if (!ok) {
            Fail("The connection was lost while sending; " + std::to_string(stored) + " of " +
                 std::to_string(total) + " covers had arrived.");
        } else {
            Status(std::to_string(stored) + " sent",
                   std::to_string(stored) + (stored == 1 ? " cover" : " covers") + " sent" +
                       (refused > 0 ? "; " + std::to_string(refused) + " could not be stored."
                                    : "."),
                   true, stored == 0);
        }
    } else if (Cancelled()) {
        Status("Stopped", "Sending covers was stopped.", true);
    }
    CloseLdn();
    s_active = false;
}

// ------------------------------------------------------------------------------ receiver

/// Takes one sender's stream. Returns false when the stream broke.
bool ReceiveStream(int fd, u32& stored, u32& total) {
    char hello[sizeof(HELLO)]{};
    if (!RecvAll(fd, hello, sizeof(hello)) || std::memcmp(hello, HELLO, sizeof(HELLO)) != 0 ||
        !RecvU32(fd, total)) {
        return false;
    }
    const std::string dir = CoverShareFolder();
    mkdir(dir.c_str(), 0777);
    mkdir((dir + "/full").c_str(), 0777);
    std::vector<u8> data;
    for (u32 seen = 0;; ++seen) {
        u8 head[2]{};
        if (!RecvAll(fd, head, sizeof(head))) {
            return false;
        }
        if (head[0] == KIND_END) {
            return true;
        }
        if (head[1] == 0 || head[1] > MAX_NAME || (head[0] != KIND_FRONT && head[0] != KIND_WRAP)) {
            return false;
        }
        std::string name(head[1], '\0');
        u32 size = 0, checksum = 0;
        if (!RecvAll(fd, name.data(), name.size()) || !RecvU32(fd, size) ||
            !RecvU32(fd, checksum) || size == 0 || size > MAX_FILE) {
            return false;
        }
        data.resize(size);
        if (!RecvAll(fd, data.data(), data.size())) {
            return false;
        }
        u8 answer = 'E';
        if (GoodName(name) && Checksum(data) == checksum) {
            const std::string path = dir + (head[0] == KIND_WRAP ? "/full/" : "/") + name;
            if (WriteFileSafely(path, data)) {
                answer = 'K';
                ++stored;
                CoverShareArrived();
            }
        }
        if (!SendAll(fd, &answer, 1)) {
            return false;
        }
        Status(std::to_string(seen + 1) + " / " + std::to_string(total), "Receiving covers",
               false);
    }
}

/// Waits for a sender on the sockets given, answering its broadcasts. Returns the accepted
/// connection, or -1 after `seconds` or when cancelled. `heard` says a sender was looking.
int WaitForSender(int listener, int udp, int seconds, bool& heard) {
    for (int waited = 0; waited < seconds * 5 && !Cancelled(); ++waited) {
        pollfd fds[2] = {{listener, POLLIN, 0}, {udp, POLLIN, 0}};
        if (poll(fds, udp >= 0 ? 2 : 1, 200) <= 0) {
            continue;
        }
        if (udp >= 0 && (fds[1].revents & POLLIN) != 0) {
            char question[16]{};
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const ssize_t n = recvfrom(udp, question, sizeof(question), 0,
                                       reinterpret_cast<sockaddr*>(&from), &length);
            if (n >= static_cast<ssize_t>(sizeof(SEEK)) &&
                std::memcmp(question, SEEK, sizeof(SEEK)) == 0) {
                heard = true;
                sendto(udp, HERE, sizeof(HERE), 0, reinterpret_cast<sockaddr*>(&from), length);
            }
        }
        if ((fds[0].revents & POLLIN) != 0) {
            const int fd = accept(listener, nullptr, nullptr);
            if (fd >= 0) {
                SetTimeouts(fd, 15);
                return fd;
            }
        }
    }
    return -1;
}

void ReceiveThread() {
    int listener = OpenListener();
    int udp = OpenUdp(true);
    int fd = -1;
    bool heard = false;
    if (listener >= 0) {
        // The network this console is already on comes first: a direct wireless link takes
        // the console off it.
        Status("Waiting", "Waiting for the other console: choose Send Covers there.", true);
        fd = WaitForSender(listener, udp, 8, heard);
        if (fd < 0 && heard && !Cancelled()) {
            fd = WaitForSender(listener, udp, 20, heard);
        }
    }
    if (fd < 0 && !Cancelled()) {
        CloseSocket(listener);
        CloseSocket(udp);
        std::string why;
        if (HostDirectWireless(why)) {
            listener = OpenListener();
            udp = OpenUdp(true);
            if (listener >= 0) {
                Status("Waiting", "Waiting for the other console nearby (direct wireless)...",
                       true);
                fd = WaitForSender(listener, udp, 180, heard);
                if (fd < 0 && !Cancelled()) {
                    Fail("No console sent covers within three minutes.");
                }
            } else {
                Fail("Covers cannot be received: the console would not open a connection.");
            }
        } else {
            Fail("Covers cannot be received: no sender on the network, and direct wireless "
                 "is not available here (" + why + ").");
        }
    }

    if (fd >= 0) {
        u32 stored = 0, total = 0;
        const bool ok = ReceiveStream(fd, stored, total);
        CloseSocket(fd);
        if (stored > 0) {
            CoverShareFinished();
        }
        if (ok) {
            Status(std::to_string(stored) + " received",
                   std::to_string(stored) + (stored == 1 ? " cover" : " covers") + " received" +
                       (stored < total ? "; " + std::to_string(total - stored) +
                                             " arrived damaged and were left out."
                                       : "."),
                   true, stored == 0 && total > 0);
        } else if (Cancelled()) {
            Status("Stopped", "Receiving covers was stopped; " + std::to_string(stored) +
                                  " were stored.", true);
        } else {
            Fail("The connection was lost while receiving; " + std::to_string(stored) +
                 " covers were stored.");
        }
    } else if (Cancelled()) {
        Status("Stopped", "Receiving covers was stopped.", true);
    }
    CloseSocket(listener);
    CloseSocket(udp);
    CloseLdn();
    s_active = false;
}

void Start(bool sending) {
    if (s_active.load()) {
        return;
    }
    Stop(); // Joins a thread that has finished.
    s_cancel = false;
    s_sending = sending;
    s_active = true;
    s_thread = std::thread{sending ? SendThread : ReceiveThread};
}

} // namespace

void StartSend() {
    Start(true);
}

void StartReceive() {
    Start(false);
}

void Stop() {
    s_cancel = true;
    if (s_thread.joinable()) {
        s_thread.join();
    }
    s_active = false;
}

bool IsActive() {
    return s_active.load();
}

bool IsSending() {
    return s_sending.load();
}

std::string Brief() {
    std::lock_guard lock{s_mutex};
    return s_brief;
}

std::string Detail() {
    std::lock_guard lock{s_mutex};
    return s_detail;
}

int Serial() {
    return s_serial.load();
}

bool Failed() {
    return s_failed.load();
}

} // namespace SwitchFrontend::CoverShare
