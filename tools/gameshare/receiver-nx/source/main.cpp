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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>

#include "citra_switch/amiibo_reader.h"
#include "citra_switch/game_share_usb.h"

#include "link_ui.h"

#include <jpeglib.h>

namespace {

constexpr u16 PORT = 28300;
constexpr char MAGIC[4] = {'D', 'K', 'G', 'S'};
constexpr std::size_t HEADER = 5;
enum PacketType : u8 {
    Hello = 1,
    Input = 2,
    Frame = 3,
    Welcome = 4,
    Command = 5,
    Audio = 6,
    Status = 7,
    Amiibo = 8,
};

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
// Local wireless (LDN). Must match src/citra_switch/game_share.cpp.
constexpr u16 LDN_SCENE_ID = 0x4453; // "DS"
constexpr u8 LDN_PASSPHRASE[0x10] = {0x44, 0x65, 0x6b, 0x6f, 0x70, 0x6f, 0x6e, 0x47,
                                     0x61, 0x6d, 0x65, 0x53, 0x68, 0x61, 0x72, 0x65};

constexpr const char* LOG_FILE = "sdmc:/switch/dekopon-link.log";
int s_log_lines = 0;

/// printf that also appends to LOG_FILE (the first few hundred lines), so the result of a
/// local wireless attempt can be read afterwards instead of off a scrolling screen.
__attribute__((format(printf, 1, 2))) void Say(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputs(line, stdout);
    if (s_log_lines < 400) {
        ++s_log_lines;
        if (std::FILE* file = std::fopen(LOG_FILE, "a")) {
            std::fputs(line, file);
            std::fclose(file);
        }
    }
}

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
    s_log_lines = 0;
    std::remove(LOG_FILE);
    bool system_service = true;
    Result rc = ldnInitialize(LdnServiceType_System);
    Say("ldnInitialize(system): 0x%x\n", rc);
    if (R_FAILED(rc)) {
        system_service = false;
        rc = ldnInitialize(LdnServiceType_User);
        Say("ldnInitialize(user): 0x%x\n", rc);
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
    Say("Scanning for a Toronja host (hold B: cancel, hold Plus: quit)...\n");
    consoleUpdate(nullptr);

    for (int round = 0; appletMainLoop() && !connected; ++round) {
        // After a full round of both protocols, try the other flavour of the service.
        if (round > 0 && round % 12 == 0) {
            ldnExit();
            station_open = false;
            system_service = !system_service;
            rc = ldnInitialize(system_service ? LdnServiceType_System : LdnServiceType_User);
            Say("switch to ldn:%s: 0x%x\n", system_service ? "s" : "u", rc);
            if (R_FAILED(rc)) {
                system_service = !system_service;
                rc = ldnInitialize(system_service ? LdnServiceType_System : LdnServiceType_User);
                Say("back to ldn:%s: 0x%x\n", system_service ? "s" : "u", rc);
                if (R_FAILED(rc)) {
                    return false;
                }
            }
        }
        // Alternate the protocol every few scans.
        if (round % 6 == 0) {
            if (station_open) {
                ldnCloseStation();
                station_open = false;
            }
            const bool mixed = (round / 6) % 2 == 0;
            rc = ldnSetProtocol(mixed ? LdnProtocol_Unknown3 : LdnProtocol_NX);
            Say("protocol %s: 0x%x\n", mixed ? "NX+Switch2" : "NX", rc);
            rc = ldnOpenStation();
            Say("ldnOpenStation: 0x%x\n", rc);
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
                Say("scan %s: 0x%x, %d network(s)\n",
                            pass == 1 ? "scene+id" : "scene only", rc, total);
                consoleUpdate(nullptr);
            }
            if (R_SUCCEEDED(rc) && total > 0) {
                Say("found: id %016llx scene %04x ch %d\n",
                            static_cast<unsigned long long>(
                                networks[0].network_id.intent_id.local_communication_id),
                            networks[0].network_id.intent_id.scene_id, networks[0].common.channel);
                rc = ldnConnect(&security, &user, 0, 0, &networks[0]);
                Say("ldnConnect[ldn:%s]: 0x%x\n", system_service ? "s" : "u", rc);
                if (rc == 0xc2cb) {
                    // The id of the host's network is checked against the NACP of the
                    // title this runs as; only applet mode has none to check.
                    Say("  -> open the Homebrew Menu from the Album, not from a game\n");
                }
                consoleUpdate(nullptr);
                connected = R_SUCCEEDED(rc);
            }
        }

        // 3) nothing of ours: say what is in range at all, once per protocol round.
        if (!connected && round % 6 == 3) {
            LdnScanFilter any{};
            s32 total = 0;
            rc = ldnScan(0, &any, networks, 0x18, &total);
            Say("scan any: 0x%x, %d network(s)\n", rc, total);
            for (s32 i = 0; i < total && i < 4; ++i) {
                Say("  id %016llx scene %04x ch %d nodes %d\n",
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
    Say("ldnGetNetworkInfo: 0x%x, ldnGetIpv4Address: 0x%x\n", rc, rc2);
    if (R_FAILED(rc) || info.node_count == 0) {
        ldnExit();
        return false;
    }
    // The first node is the access point, i.e. the Dekopon host.
    host_addr.s_addr = htonl(info.nodes[0].ip_addr.addr);
    Say("Joined. Host %s\n", inet_ntoa(host_addr));
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
            std::fputs("# Toronja Link: addresses of Toronja hosts to try, one per line.\n"
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

    // Motion, for games that aim with it. 0: handheld, 1: Pro Controller, 2-3: Joy-Con pair.
    HidSixAxisSensorHandle sensors[4]{};
    hidGetSixAxisSensorHandles(&sensors[0], 1, HidNpadIdType_Handheld,
                               HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&sensors[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&sensors[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    for (const HidSixAxisSensorHandle& sensor : sensors) {
        hidStartSixAxisSensor(sensor);
    }

    consoleInit(nullptr);
    std::printf("Toronja Link\n\n");
    std::printf("Looking for a Toronja host on this network...\n");
    std::printf("(open a game in Toronja on the other console)\n\n");
    std::printf("Both stick clicks: bottom / top screen\n");
    std::printf("Plus: quit (while streaming: hold Plus + Minus)\n");
    consoleUpdate(nullptr);
    bool console_active = true;
    Framebuffer fb{};

    // How to reach the host. The socket is created afterwards, so that with local wireless it
    // lives on that network.
    std::printf("\nA: network (Wi-Fi / LAN)    X: local wireless (direct)\n");
    std::printf("Local wireless needs the Homebrew Menu opened from the Album,\n"
                "not on top of a game.\n");
    consoleUpdate(nullptr);
    bool local_wireless = false;
    bool usb_mode = false;
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
#ifdef DEKOPON_LINK_USB
        if (down & HidNpadButton_Y) {
            char usb_log[256];
            usb_mode = GameShareUsb::Start(usb_log, sizeof(usb_log));
            s_log_lines = 0;
            std::remove(LOG_FILE);
            Say("USB: %s\n", usb_log);
            if (!usb_mode) {
                std::printf("USB is not available. A: network  X: local wireless\n");
                consoleUpdate(nullptr);
                continue;
            }
            std::printf("Connect both consoles with a USB-C cable.\n");
            consoleUpdate(nullptr);
            break;
        }
#endif
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

    Link::Screen ui;
    ui.Init();
    Link::AudioPlayer audio;
    bool amiibo_scanning = false;
    bool amiibo_sent = false;
    bool amiibo_refused = false;
    u64 last_amiibo_poll_ns = 0;
    bool touch_was_ours = false;
    u32 input_seq = 0;
    u64 last_hello_ns = 0;
    u64 last_input_ns = 0;
    u32 ack_id = 0; // newest frame id seen, reported back to the host
    bool usb_was_connected = false;
    u64 last_usb_status_ns = 0;
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
            } else if (now - quit_since_ns > 3'000'000'000ULL) {
                break;
            } else if (now - quit_since_ns > 400'000'000ULL && !ui.IsOpen() && !console_active) {
                ui.Open();
            }
        } else {
            quit_since_ns = 0;
        }

        // Both sticks pressed in: the right stick alone belongs to the game.
        constexpr u64 both_sticks = HidNpadButton_StickL | HidNpadButton_StickR;
        if ((held & both_sticks) == both_sticks && (pressed & both_sticks) != 0) {
            ui.ToggleView();
        }
        ui.HandlePad(pressed);
        if (ui.WantsQuit()) {
            break;
        }
        if (ui.TakeHelloDue()) {
            last_hello_ns = 0; // say what changed right away
        }

        if (usb_mode && console_active &&
            GameShareUsb::IsConnected() != usb_was_connected) {
            usb_was_connected = !usb_was_connected;
            Say(usb_was_connected ? "USB link up (this console is the %s)\n"
                                  : "USB link down%s\n",
                GameShareUsb::Role());
            consoleUpdate(nullptr);
        }
        // Until a picture arrives, say what the link sees every two seconds (also in
        // the log file).
        if (usb_mode && console_active && now - last_usb_status_ns >= 2'000'000'000ULL) {
            last_usb_status_ns = now;
            char status[320];
            GameShareUsb::Status(status, sizeof(status));
            Say("%s | host answered: %s\n", status, host_known ? "yes" : "no");
            consoleUpdate(nullptr);
        }

        if (sock >= 0 || usb_mode) {
            // HELLO once a second: finds the host (broadcast) and keeps the stream alive.
            if (now - last_hello_ns >= 1'000'000'000ULL) {
                last_hello_ns = now;
                u8 hello[HEADER + 4];
                std::memcpy(hello, MAGIC, sizeof(MAGIC));
                hello[4] = PacketType::Hello;
                hello[5] = ui.RequestedScreen();
                hello[6] = 80; // JPEG quality
                hello[7] = 30; // frames per second
                hello[8] = ui.settings.audio ? 1 : 0; // the sound too
                if (usb_mode) {
                    GameShareUsb::Send(hello, sizeof(hello));
                } else {
                    sendto(sock, hello, sizeof(hello), 0,
                           reinterpret_cast<const sockaddr*>(&host), sizeof(host));
                }
                if (!host_known && !usb_mode) {
                    // Not found by broadcast yet: also ask the listed addresses directly.
                    for (const in_addr& extra : extra_hosts) {
                        sockaddr_in target = host;
                        target.sin_addr = extra;
                        sendto(sock, hello, sizeof(hello), 0,
                               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
                    }
                }
            }

            // At most 125 times a second: one packet per pass of this loop (up to a
            // thousand a second) floods a wireless link with tiny packets.
            if (host_known && now - last_input_ns >= 8'000'000ULL) {
                last_input_ns = now;
                u32 buttons = 0;
                for (const ButtonMap& map : BUTTONS) {
                    if (!ui.IsOpen() && (held & map.pad)) {
                        buttons |= 1U << map.bit;
                    }
                }
                HidAnalogStickState stick = padGetStickPos(&pad, 0);
                HidAnalogStickState right = padGetStickPos(&pad, 1);
                if (ui.IsOpen()) {
                    stick = {};
                    right = {};
                }
                u8 touching = 0;
                u16 touch_x = 0, touch_y = 0;
                HidTouchScreenState touch{};
                const bool finger = hidGetTouchScreenStates(&touch, 1) != 0 && touch.count > 0;
                const int finger_x = finger ? static_cast<int>(touch.touches[0].x) : 0;
                const int finger_y = finger ? static_cast<int>(touch.touches[0].y) : 0;
                // The tab and the menu get the touch first.
                const bool ours = !console_active && ui.HandleTouch(finger, finger_x, finger_y);
                touch_was_ours = finger ? (touch_was_ours || ours) : false;
                Link::Rect bottom_rect{};
                if (finger && !touch_was_ours && !ui.IsOpen() && picture.width != 0 &&
                    ui.BottomScreenRect(static_cast<int>(picture.width),
                                        static_cast<int>(picture.height), bottom_rect) &&
                    bottom_rect.Contains(finger_x, finger_y)) {
                    touching = 1;
                    touch_x = static_cast<u16>((finger_x - bottom_rect.x) * 320 / bottom_rect.w);
                    touch_y = static_cast<u16>((finger_y - bottom_rect.y) * 240 / bottom_rect.h);
                }

                u8 packet[HEADER + 50];
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
                WriteU32(packet + HEADER + 21, ack_id);
                HidSixAxisSensorState motion{};
                const u32 style = padGetStyleSet(&pad);
                bool has_motion = false;
                if (style & HidNpadStyleTag_NpadHandheld) {
                    has_motion = hidGetSixAxisSensorStates(sensors[0], &motion, 1) > 0;
                } else if (style & HidNpadStyleTag_NpadFullKey) {
                    has_motion = hidGetSixAxisSensorStates(sensors[1], &motion, 1) > 0;
                } else if (style & HidNpadStyleTag_NpadJoyDual) {
                    has_motion = hidGetSixAxisSensorStates(sensors[3], &motion, 1) > 0 ||
                                 hidGetSixAxisSensorStates(sensors[2], &motion, 1) > 0;
                }
                const float values[6] = {motion.acceleration.x,     motion.acceleration.y,
                                         motion.acceleration.z,     motion.angular_velocity.x,
                                         motion.angular_velocity.y, motion.angular_velocity.z};
                packet[HEADER + 25] = has_motion ? 1 : 0;
                std::memcpy(packet + HEADER + 26, values, sizeof(values));
                if (usb_mode) {
                    GameShareUsb::Send(packet, sizeof(packet));
                } else {
                    sendto(sock, packet, sizeof(packet), 0,
                           reinterpret_cast<const sockaddr*>(&host), sizeof(host));
                }
            }

            // Take everything that has arrived; only the newest complete frame is shown.
            for (;;) {
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const ssize_t got =
                    usb_mode ? static_cast<ssize_t>(
                                   GameShareUsb::Receive(recv_buffer, sizeof(recv_buffer)))
                             : recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
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
                if (type == PacketType::Status && size >= HEADER + 2) {
                    ui.OnStatus(body[0]);
                } else if (type == PacketType::Audio && size >= HEADER + 6 + 4) {
                    const std::size_t frames = ReadU16(body + 4);
                    if (ui.settings.audio && size >= HEADER + 6 + frames * 4 && audio.Start()) {
                        // The samples sit unaligned in the packet.
                        static s16 pcm[512];
                        const std::size_t count = std::min<std::size_t>(frames * 2, 512);
                        std::memcpy(pcm, body + 6, count * sizeof(s16));
                        audio.Push(pcm, count / 2);
                    }
                } else if (type == PacketType::Welcome) {
                    if (!host_known) {
                        ui.QueueHostState(); // a host just appeared: tell it how we are set up
                        host.sin_addr = from.sin_addr;
                        host_known = true;
                        if (console_active) {
                            std::printf("\nHost found at %s\n", inet_ntoa(from.sin_addr));
                            consoleUpdate(nullptr);
                        }
                    }
                } else if (type == PacketType::Frame && size > HEADER + 17 && host_known) {
                    const u32 id = ReadU32(body);
                    if (static_cast<s32>(id - ack_id) > 0) {
                        ack_id = id;
                    }
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
                                ui.CountFrame(now);
                            }
                        }
                    }
                }
            }
        }

        // What the menu asked the host for. Twice: these are datagrams.
        if (host_known) {
            for (const Link::Command& command : ui.TakeCommands()) {
                u8 out[HEADER + 2];
                std::memcpy(out, MAGIC, sizeof(MAGIC));
                out[4] = PacketType::Command;
                out[5] = command.what;
                out[6] = command.value;
                for (int i = 0; i < 2; ++i) {
                    if (usb_mode) {
                        GameShareUsb::Send(out, sizeof(out));
                    } else if (sock >= 0) {
                        sendto(sock, out, sizeof(out), 0, reinterpret_cast<const sockaddr*>(&host),
                               sizeof(host));
                    }
                }
            }
        }
        if (!ui.settings.audio) {
            audio.Stop();
        }
        audio.Pump();

        // The game on the other console is asking for an Amiibo: offer this console's
        // reader. Every step can fail (no reader, Joy-Con detached, figure pulled away);
        // a failure ends the attempt with a notice and nothing else.
        if (host_known && !console_active && now - last_amiibo_poll_ns >= 250'000'000ULL) {
            last_amiibo_poll_ns = now;
            const bool wanted = ui.AmiiboWanted();
            if (!wanted) {
                if (amiibo_scanning) {
                    SwitchFrontend::ReleaseAmiibo();
                    amiibo_scanning = false;
                }
                amiibo_sent = false;
                amiibo_refused = false;
            } else if (!amiibo_scanning && !amiibo_sent && !amiibo_refused) {
                std::string error;
                if (SwitchFrontend::IsAmiiboReaderAvailable() &&
                    SwitchFrontend::StartAmiiboScan(error)) {
                    amiibo_scanning = true;
                    ui.Notify("The game wants an Amiibo: hold it against the right stick", now,
                              5'000'000'000ULL);
                } else {
                    amiibo_refused = true; // Once per request, not every quarter second.
                    ui.Notify(error.empty() ? "No NFC reader here: scan the Amiibo on the other console"
                                            : "Amiibo reader: " + error,
                              now, 4'000'000'000ULL);
                }
            } else if (amiibo_scanning && !amiibo_sent) {
                SwitchFrontend::AmiiboCapture capture;
                std::string error;
                if (SwitchFrontend::PollAmiiboScan(capture, error)) {
                    constexpr std::size_t NFP = 0x298;
                    u8 out[HEADER + NFP + 14]{};
                    std::memcpy(out, MAGIC, sizeof(MAGIC));
                    out[4] = PacketType::Amiibo;
                    std::memcpy(out + HEADER, capture.nfp_data.data(), NFP);
                    std::memcpy(out + HEADER + NFP, capture.uid.data(), 7);
                    WriteU16(out + HEADER + NFP + 7, capture.character_id);
                    out[HEADER + NFP + 9] = capture.character_variant;
                    out[HEADER + NFP + 10] = capture.amiibo_type;
                    WriteU16(out + HEADER + NFP + 11, capture.model_number);
                    out[HEADER + NFP + 13] = capture.series;
                    for (int i = 0; i < 3; ++i) {
                        if (usb_mode) {
                            GameShareUsb::Send(out, sizeof(out));
                        } else if (sock >= 0) {
                            sendto(sock, out, sizeof(out), 0,
                                   reinterpret_cast<const sockaddr*>(&host), sizeof(host));
                        }
                    }
                    amiibo_sent = true;
                    SwitchFrontend::ReleaseAmiibo();
                    amiibo_scanning = false;
                    ui.Notify(capture.name.empty() ? "Amiibo sent to the game"
                                                   : capture.name + " sent to the game",
                              now);
                } else if (!error.empty()) {
                    SwitchFrontend::ReleaseAmiibo();
                    amiibo_scanning = false;
                    amiibo_refused = true;
                    ui.Notify("Amiibo read failed: " + error, now, 4'000'000'000ULL);
                }
            }
        }

        if (!console_active && !picture_dirty && ui.NeedsRedraw(now)) {
            picture_dirty = true; // The menu, the tab or a notice changed.
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
            u32 stride = 0;
            u32* pixels = static_cast<u32*>(framebufferBegin(&fb, &stride));
            ui.Compose(pixels, stride / sizeof(u32),
                       picture.width != 0 ? picture.rgb.data() : nullptr,
                       static_cast<int>(picture.width), static_cast<int>(picture.height));
            framebufferEnd(&fb);
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

    if (amiibo_scanning) {
        SwitchFrontend::ReleaseAmiibo();
    }
    audio.Stop();
    ui.settings.Save();
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
    if (usb_mode) {
        GameShareUsb::Stop();
    }
    socketExit();
    return 0;
}
