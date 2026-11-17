// SPDX-License-Identifier: GPL-2.0-or-later
//
// Dekopon Link: receiver for Dekopon's GameShare prototype, for Switch 1 (libnx) and
// Switch 2 (libounce). Shows the 3DS screen a Dekopon host shares over the local network and
// sends this console's buttons, stick and touch screen back.
//
// Protocol: see src/citra_switch/game_share.h in the Dekopon repository.
//
// The host is found by broadcast. When the two consoles are not on the same subnet (for
// example this one on a PC's Wi-Fi hotspot and the host on that PC's Ethernet), list the
// host's address in sdmc:/switch/dekopon-link.txt, one IPv4 address per line.
//
// Controls:  everything is sent to the game, except
//   Both stick clicks      switch between the bottom and the top screen
//   Plus                   quit, until a picture arrives
//   Plus + Minus (hold)    quit while streaming

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>

#include <jpeglib.h>

namespace {

constexpr u16 PORT = 28300;
constexpr char MAGIC[4] = {'D', 'K', 'G', 'S'};
constexpr std::size_t HEADER = 5;
enum PacketType : u8 { Hello = 1, Input = 2, Frame = 3, Welcome = 4 };

constexpr u32 FB_WIDTH = 1280;
constexpr u32 FB_HEIGHT = 720;

// Bit positions follow SwitchFrontend::InputButton in Dekopon.
struct ButtonMap {
    u64 pad;
    u32 bit;
};
constexpr ButtonMap BUTTONS[] = {
    {HidNpadButton_A, 0},     {HidNpadButton_B, 1},      {HidNpadButton_X, 2},
    {HidNpadButton_Y, 3},     {HidNpadButton_Up, 4},     {HidNpadButton_Down, 5},
    {HidNpadButton_Left, 6},  {HidNpadButton_Right, 7},  {HidNpadButton_L, 8},
    {HidNpadButton_R, 9},     {HidNpadButton_Plus, 10},  {HidNpadButton_Minus, 11},
    {HidNpadButton_ZL, 12},   {HidNpadButton_ZR, 13},
};

void WriteU16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void WriteU32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}
u16 ReadU16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 ReadU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

// ---------------------------------------------------------------------- JPEG decoding
struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
};

void OnJpegError(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1);
}

struct Picture {
    u32 width{};
    u32 height{};
    std::vector<u8> rgb;
};

bool DecodeJpeg(const std::vector<u8>& data, Picture& out) {
    jpeg_decompress_struct cinfo{};
    JpegError error{};
    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = OnJpegError;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data.data(), static_cast<unsigned long>(data.size()));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width > 1024 || cinfo.output_height > 1024 || cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    out.width = cinfo.output_width;
    out.height = cinfo.output_height;
    out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = out.rgb.data() + static_cast<std::size_t>(cinfo.output_scanline) * out.width * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

// ------------------------------------------------------------------------- on screen
struct Placement {
    u32 scale{1};
    u32 left{};
    u32 top{};
};

Placement Place(u32 width, u32 height) {
    Placement p;
    p.scale = std::max<u32>(1, std::min(FB_WIDTH / std::max<u32>(width, 1),
                                        FB_HEIGHT / std::max<u32>(height, 1)));
    p.left = (FB_WIDTH - width * p.scale) / 2;
    p.top = (FB_HEIGHT - height * p.scale) / 2;
    return p;
}

void Draw(Framebuffer& fb, const Picture& picture) {
    u32 stride = 0;
    u32* pixels = static_cast<u32*>(framebufferBegin(&fb, &stride));
    const u32 row_pixels = stride / sizeof(u32);
    std::memset(pixels, 0, static_cast<std::size_t>(stride) * FB_HEIGHT);
    if (picture.width != 0) {
        const Placement p = Place(picture.width, picture.height);
        for (u32 y = 0; y < picture.height; ++y) {
            const u8* src = picture.rgb.data() + static_cast<std::size_t>(y) * picture.width * 3;
            u32* first_row = pixels + static_cast<std::size_t>(p.top + y * p.scale) * row_pixels + p.left;
            u32* dst = first_row;
            for (u32 x = 0; x < picture.width; ++x, src += 3) {
                const u32 color = RGBA8_MAXALPHA(src[0], src[1], src[2]);
                for (u32 k = 0; k < p.scale; ++k) {
                    *dst++ = color;
                }
            }
            for (u32 k = 1; k < p.scale; ++k) {
                std::memcpy(first_row + static_cast<std::size_t>(k) * row_pixels, first_row,
                            static_cast<std::size_t>(picture.width) * p.scale * sizeof(u32));
            }
        }
    }
    framebufferEnd(&fb);
}

// Local wireless (LDN). Must match src/citra_switch/game_share.cpp.
constexpr u16 LDN_SCENE_ID = 0x4453; // "DS"
constexpr u8 LDN_PASSPHRASE[0x10] = {0x44, 0x65, 0x6b, 0x6f, 0x70, 0x6f, 0x6e, 0x47,
                                     0x61, 0x6d, 0x65, 0x53, 0x68, 0x61, 0x72, 0x65};

/// Joins the local network a Dekopon host created and returns the host's address. Prints
/// every step with its result code: this path cannot be tested without two consoles.
///
/// Two things are not known in advance, so both are tried in turn:
///  - the protocol (plain NX, or the mixed NX + Switch 2 one);
///  - the filter: a network only matches "our" local communication id when both consoles run
///    the homebrew through the same title, so the scan also looks for the scene id alone and,
///    failing that, lists whatever local networks are in range.
bool JoinLocalWireless(PadState& pad, in_addr& host_addr, bool& quit) {
    // The system flavour of the service first: the user one only lets an application join
    // networks whose local communication id is listed by the title it runs as, and the host
    // runs as a different title (ldnConnect then fails with 0xc2cb, "invalid object").
    Result rc = ldnInitialize(LdnServiceType_System);
    std::printf("ldnInitialize(system): 0x%x\n", rc);
    if (R_FAILED(rc)) {
        rc = ldnInitialize(LdnServiceType_User);
        std::printf("ldnInitialize(user): 0x%x\n", rc);
    }
    consoleUpdate(nullptr);
    if (R_FAILED(rc)) {
        return false;
    }

    LdnSecurityConfig security{};
    security.security_mode = LdnSecurityMode_Product;
    security.passphrase_size = sizeof(LDN_PASSPHRASE);
    std::memcpy(security.passphrase, LDN_PASSPHRASE, sizeof(LDN_PASSPHRASE));
    LdnUserConfig user{};
    std::strncpy(user.user_name, "DekoponLink", sizeof(user.user_name) - 1);

    static LdnNetworkInfo networks[0x18];
    bool station_open = false;
    bool connected = false;
    std::printf("Scanning for a Dekopon host (hold B: cancel, hold Plus: quit)...\n");
    consoleUpdate(nullptr);

    for (int round = 0; appletMainLoop() && !connected; ++round) {
        // Alternate the protocol every few scans.
        if (round % 6 == 0) {
            if (station_open) {
                ldnCloseStation();
                station_open = false;
            }
            const bool mixed = (round / 6) % 2 == 0;
            rc = ldnSetProtocol(mixed ? LdnProtocol_Unknown3 : LdnProtocol_NX);
            std::printf("protocol %s: 0x%x\n", mixed ? "NX+Switch2" : "NX", rc);
            rc = ldnOpenStation();
            std::printf("ldnOpenStation: 0x%x\n", rc);
            consoleUpdate(nullptr);
            if (R_FAILED(rc)) {
                ldnExit();
                return false;
            }
            station_open = true;
        }

        // Held, not "just pressed": a scan blocks for a while, and a short press in between
        // two pad updates would go unnoticed.
        padUpdate(&pad);
        if (padGetButtons(&pad) & (HidNpadButton_B | HidNpadButton_Plus)) {
            quit = (padGetButtons(&pad) & HidNpadButton_Plus) != 0;
            ldnExit();
            return false;
        }

        // 1) our scene id and our local communication id; 2) our scene id only.
        for (int pass = 0; pass < 2 && !connected; ++pass) {
            LdnScanFilter filter{};
            filter.network_id.intent_id.local_communication_id = -1;
            filter.network_id.intent_id.scene_id = LDN_SCENE_ID;
            filter.flags = LdnScanFilterFlag_SceneId |
                           (pass == 1 ? LdnScanFilterFlag_LocalCommunicationId : 0);
            s32 total = 0;
            rc = ldnScan(0, &filter, networks, 0x18, &total);
            if (R_FAILED(rc) || total > 0) {
                std::printf("scan %s: 0x%x, %d network(s)\n",
                            pass == 1 ? "scene+id" : "scene only", rc, total);
                consoleUpdate(nullptr);
            }
            if (R_SUCCEEDED(rc) && total > 0) {
                std::printf("found: id %016llx scene %04x ch %d\n",
                            static_cast<unsigned long long>(
                                networks[0].network_id.intent_id.local_communication_id),
                            networks[0].network_id.intent_id.scene_id, networks[0].common.channel);
                rc = ldnConnect(&security, &user, 0, 0, &networks[0]);
                std::printf("ldnConnect: 0x%x\n", rc);
                consoleUpdate(nullptr);
                connected = R_SUCCEEDED(rc);
            }
        }

        // 3) nothing of ours: say what is in range at all, once per protocol round.
        if (!connected && round % 6 == 3) {
            LdnScanFilter any{};
            s32 total = 0;
            rc = ldnScan(0, &any, networks, 0x18, &total);
            std::printf("scan any: 0x%x, %d network(s)\n", rc, total);
            for (s32 i = 0; i < total && i < 4; ++i) {
                std::printf("  id %016llx scene %04x ch %d nodes %d\n",
                            static_cast<unsigned long long>(
                                networks[i].network_id.intent_id.local_communication_id),
                            networks[i].network_id.intent_id.scene_id, networks[i].common.channel,
                            networks[i].node_count);
            }
            consoleUpdate(nullptr);
        }

        for (int wait = 0; wait < 6 && !connected; ++wait) {
            svcSleepThread(50'000'000);
            padUpdate(&pad);
            if (padGetButtons(&pad) & (HidNpadButton_B | HidNpadButton_Plus)) {
                quit = (padGetButtons(&pad) & HidNpadButton_Plus) != 0;
                ldnExit();
                return false;
            }
        }
    }
    if (!connected) {
        ldnExit();
        return false;
    }

    LdnNetworkInfo info{};
    rc = ldnGetNetworkInfo(&info);
    LdnIpv4Address address{};
    LdnSubnetMask mask{};
    const Result rc2 = ldnGetIpv4Address(&address, &mask);
    std::printf("ldnGetNetworkInfo: 0x%x, ldnGetIpv4Address: 0x%x\n", rc, rc2);
    if (R_FAILED(rc) || info.node_count == 0) {
        ldnExit();
        return false;
    }
    // The first node is the access point, i.e. the Dekopon host.
    host_addr.s_addr = htonl(info.nodes[0].ip_addr.addr);
    std::printf("Joined. Host %s\n", inet_ntoa(host_addr));
    consoleUpdate(nullptr);
    return true;
}

constexpr const char* HOSTS_FILE = "sdmc:/switch/dekopon-link.txt";

/// Extra addresses to try besides the broadcast. Creates the file with an example the first
/// time.
std::vector<in_addr> LoadHosts() {
    std::vector<in_addr> hosts;
    std::FILE* file = std::fopen(HOSTS_FILE, "r");
    if (file == nullptr) {
        file = std::fopen(HOSTS_FILE, "w");
        if (file != nullptr) {
            std::fputs("# Dekopon Link: addresses of Dekopon hosts to try, one per line.\n"
                       "# Only needed when the host is not found automatically.\n"
                       "192.168.0.100\n",
                       file);
            std::fclose(file);
        }
        in_addr fallback{};
        if (inet_aton("192.168.0.100", &fallback) != 0) {
            hosts.push_back(fallback);
        }
        return hosts;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* end = line + std::strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }
        in_addr addr{};
        if (line[0] != '#' && line[0] != '\0' && inet_aton(line, &addr) != 0) {
            hosts.push_back(addr);
        }
    }
    std::fclose(file);
    return hosts;
}

} // namespace

int main(int, char**) {
    socketInitializeDefault();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    consoleInit(nullptr);
    std::printf("Dekopon Link\n\n");
    std::printf("Looking for a Dekopon host on this network...\n");
    std::printf("(open a game in Dekopon on the other console)\n\n");
    std::printf("Both stick clicks: bottom / top screen\n");
    std::printf("Plus: quit (while streaming: hold Plus + Minus)\n");
    consoleUpdate(nullptr);
    bool console_active = true;
    Framebuffer fb{};

    // How to reach the host. The socket is created afterwards, so that with local wireless it
    // lives on that network.
    std::printf("\nA: network (Wi-Fi / LAN)    X: local wireless (direct)\n");
    consoleUpdate(nullptr);
    bool local_wireless = false;
    bool quit = false;
    in_addr local_host{};
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) {
            quit = true;
            break;
        }
        if (down & HidNpadButton_A) {
            break;
        }
        if (down & HidNpadButton_X) {
            local_wireless = JoinLocalWireless(pad, local_host, quit);
            if (quit) {
                break;
            }
            if (!local_wireless) {
                // Let go of B first, or it would not register as a new press below.
                std::printf("Local wireless stopped. A: network  X: try again  Plus: quit\n");
                consoleUpdate(nullptr);
                continue;
            }
            break;
        }
        consoleUpdate(nullptr);
    }

    if (quit) {
        consoleExit(nullptr);
        socketExit();
        return 0;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        const int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    }

    sockaddr_in host{};
    host.sin_family = AF_INET;
    host.sin_port = htons(PORT);
    host.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bool host_known = false;
    if (local_wireless) {
        // The host is the network's access point; ask it directly.
        host.sin_addr = local_host;
    }
    const std::vector<in_addr> extra_hosts = LoadHosts();

    u8 screen = 1; // bottom
    u32 input_seq = 0;
    u64 last_hello_ns = 0;
    u64 last_frame_ns = 0;
    u64 quit_since_ns = 0;

    u32 frame_id = 0;
    u16 chunk_count = 0;
    u16 chunks_seen = 0;
    std::vector<u8> assembly;
    std::vector<u8> have_chunk;
    std::vector<u8> jpeg;
    Picture picture;
    bool picture_dirty = false;
    u8 recv_buffer[2048];

    while (appletMainLoop()) {
        const u64 now = armTicksToNs(armGetSystemTick());
        padUpdate(&pad);
        const u64 held = padGetButtons(&pad);
        const u64 pressed = padGetButtonsDown(&pad);

        // Quit: Plus alone while still looking for the host (nothing to forward it to yet);
        // once streaming, Plus + Minus held for half a second.
        if (console_active && (pressed & HidNpadButton_Plus)) {
            break;
        }
        if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus)) {
            if (quit_since_ns == 0) {
                quit_since_ns = now;
            } else if (now - quit_since_ns > 500'000'000ULL) {
                break;
            }
        } else {
            quit_since_ns = 0;
        }

        // Both sticks pressed in: the right stick alone belongs to the game.
        constexpr u64 both_sticks = HidNpadButton_StickL | HidNpadButton_StickR;
        if ((held & both_sticks) == both_sticks && (pressed & both_sticks) != 0) {
            screen = screen == 1 ? 0 : 1;
            last_hello_ns = 0; // ask for the other screen right away
        }

        if (sock >= 0) {
            // HELLO once a second: finds the host (broadcast) and keeps the stream alive.
            if (now - last_hello_ns >= 1'000'000'000ULL) {
                last_hello_ns = now;
                u8 hello[HEADER + 3];
                std::memcpy(hello, MAGIC, sizeof(MAGIC));
                hello[4] = PacketType::Hello;
                hello[5] = screen;
                hello[6] = 80; // JPEG quality
                hello[7] = 30; // frames per second
                sendto(sock, hello, sizeof(hello), 0, reinterpret_cast<const sockaddr*>(&host),
                       sizeof(host));
                if (!host_known) {
                    // Not found by broadcast yet: also ask the listed addresses directly.
                    for (const in_addr& extra : extra_hosts) {
                        sockaddr_in target = host;
                        target.sin_addr = extra;
                        sendto(sock, hello, sizeof(hello), 0,
                               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
                    }
                }
            }

            if (host_known) {
                u32 buttons = 0;
                for (const ButtonMap& map : BUTTONS) {
                    if (held & map.pad) {
                        buttons |= 1U << map.bit;
                    }
                }
                const HidAnalogStickState stick = padGetStickPos(&pad, 0);
                const HidAnalogStickState right = padGetStickPos(&pad, 1);
                u8 touching = 0;
                u16 touch_x = 0, touch_y = 0;
                HidTouchScreenState touch{};
                if (screen == 1 && picture.width != 0 &&
                    hidGetTouchScreenStates(&touch, 1) != 0 && touch.count > 0) {
                    const Placement p = Place(picture.width, picture.height);
                    const s32 x = (static_cast<s32>(touch.touches[0].x) - static_cast<s32>(p.left)) /
                                  static_cast<s32>(p.scale);
                    const s32 y = (static_cast<s32>(touch.touches[0].y) - static_cast<s32>(p.top)) /
                                  static_cast<s32>(p.scale);
                    if (x >= 0 && y >= 0 && x < static_cast<s32>(picture.width) &&
                        y < static_cast<s32>(picture.height)) {
                        touching = 1;
                        touch_x = static_cast<u16>(x);
                        touch_y = static_cast<u16>(y);
                    }
                }

                u8 packet[HEADER + 21];
                std::memcpy(packet, MAGIC, sizeof(MAGIC));
                packet[4] = PacketType::Input;
                WriteU32(packet + HEADER, ++input_seq);
                WriteU32(packet + HEADER + 4, buttons);
                WriteU16(packet + HEADER + 8, static_cast<u16>(static_cast<s16>(stick.x)));
                WriteU16(packet + HEADER + 10, static_cast<u16>(static_cast<s16>(stick.y)));
                packet[HEADER + 12] = touching;
                WriteU16(packet + HEADER + 13, touch_x);
                WriteU16(packet + HEADER + 15, touch_y);
                WriteU16(packet + HEADER + 17, static_cast<u16>(static_cast<s16>(right.x)));
                WriteU16(packet + HEADER + 19, static_cast<u16>(static_cast<s16>(right.y)));
                sendto(sock, packet, sizeof(packet), 0, reinterpret_cast<const sockaddr*>(&host),
                       sizeof(host));
            }

            // Take everything that has arrived; only the newest complete frame is shown.
            for (;;) {
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const ssize_t got = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                             reinterpret_cast<sockaddr*>(&from), &from_len);
                if (got <= 0) {
                    break;
                }
                const std::size_t size = static_cast<std::size_t>(got);
                if (size < HEADER || std::memcmp(recv_buffer, MAGIC, sizeof(MAGIC)) != 0) {
                    continue;
                }
                const u8 type = recv_buffer[4];
                const u8* body = recv_buffer + HEADER;
                if (type == PacketType::Welcome) {
                    if (!host_known) {
                        host.sin_addr = from.sin_addr;
                        host_known = true;
                        if (console_active) {
                            std::printf("\nHost found at %s\n", inet_ntoa(from.sin_addr));
                            consoleUpdate(nullptr);
                        }
                    }
                } else if (type == PacketType::Frame && size > HEADER + 17 && host_known) {
                    const u32 id = ReadU32(body);
                    const u16 index = ReadU16(body + 4);
                    const u16 count = ReadU16(body + 6);
                    const u32 total = ReadU32(body + 8);
                    const std::size_t payload = size - HEADER - 17;
                    constexpr std::size_t CHUNK = 1200;
                    if (count == 0 || index >= count || total == 0 || total > 512 * 1024 ||
                        static_cast<std::size_t>(index) * CHUNK + payload > total) {
                        continue;
                    }
                    if (id != frame_id || count != chunk_count || assembly.size() != total) {
                        frame_id = id;
                        chunk_count = count;
                        chunks_seen = 0;
                        assembly.assign(total, 0);
                        have_chunk.assign(count, 0);
                    }
                    if (!have_chunk[index]) {
                        have_chunk[index] = 1;
                        ++chunks_seen;
                        std::memcpy(assembly.data() + static_cast<std::size_t>(index) * CHUNK,
                                    body + 17, payload);
                        if (chunks_seen == chunk_count) {
                            jpeg = assembly;
                            if (DecodeJpeg(jpeg, picture)) {
                                picture_dirty = true;
                                last_frame_ns = now;
                            }
                        }
                    }
                }
            }
        }

        if (picture_dirty) {
            if (console_active) {
                consoleExit(nullptr);
                console_active = false;
                framebufferCreate(&fb, nwindowGetDefault(), FB_WIDTH, FB_HEIGHT,
                                  PIXEL_FORMAT_RGBA_8888, 2);
                framebufferMakeLinear(&fb);
            }
            picture_dirty = false;
            Draw(fb, picture);
        } else if (console_active) {
            consoleUpdate(nullptr);
        } else {
            // Nothing new: wait a moment instead of spinning. If the stream has stopped for a
            // while, forget the host so the next HELLO goes out as a broadcast again.
            if (host_known && !local_wireless && now - last_frame_ns > 5'000'000'000ULL) {
                host_known = false;
                host.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            }
            svcSleepThread(1'000'000);
        }
    }

    if (sock >= 0) {
        close(sock);
    }
    if (console_active) {
        consoleExit(nullptr);
    } else {
        framebufferClose(&fb);
    }
    if (local_wireless) {
        ldnExit();
    }
    socketExit();
    return 0;
}
