// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <jpeglib.h>

#include "audio_core/dsp_interface.h"
#include "citra_switch/amiibo_reader.h"
#include "citra_switch/config.h"
#include "citra_switch/emu_window.h"
#include "citra_switch/game_share.h"
#include "citra_switch/game_share_ldn.h"
#include "citra_switch/game_share_usb.h"
#include "citra_switch/input.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace SwitchFrontend::GameShare {
namespace {

using Clock = std::chrono::steady_clock;

constexpr u16 PORT = 28300;
constexpr char MAGIC[4] = {'D', 'K', 'G', 'S'};
constexpr std::size_t HEADER = 5; // magic + type
constexpr std::size_t CHUNK = 1200;
constexpr auto CLIENT_TIMEOUT = std::chrono::seconds{3};
constexpr auto INPUT_TIMEOUT = std::chrono::milliseconds{500};
constexpr auto STATS_INTERVAL = std::chrono::seconds{5};
// Congestion control: frames sent but not yet seen by the receiver before holding back,
// and how often to send one anyway to find out whether the link has recovered.
constexpr u32 MAX_UNACKED_FRAMES = 3;
constexpr auto PROBE_INTERVAL = std::chrono::milliseconds{250};

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

struct RemoteInput {
    u32 buttons{};
    s16 stick_x{};
    s16 stick_y{};
    s16 right_x{};
    s16 right_y{};
    bool touching{};
    u16 touch_x{};
    u16 touch_y{};
    bool has_motion{};
    float motion[6]{}; // acceleration x y z, angular velocity x y z
    Clock::time_point when{};
};

struct Shared {
    std::mutex mutex;
    // Receiver
    bool client_active{};
    sockaddr_in client_addr{};
    u8 screen{1};         // 0 top, 1 bottom, 2 both (top above bottom)
    bool audio_wanted{};  // The receiver asked for the sound too.
    u8 quality{80};
    u8 max_fps{30};
    Clock::time_point client_seen{};
    RemoteInput input{};
    // Frame hand-over: renderer -> network thread
    bool capture_pending{};
    Clock::time_point capture_requested{};
    Clock::time_point last_capture{};
    std::vector<u8> capture; // written by the renderer (BGRA)
    std::vector<u8> ready;   // copy for the network thread
    u32 ready_width{};
    u32 ready_height{};
    u8 ready_screen{};
    bool frame_ready{};
    u64 capture_micros{};
};

Shared s;
std::atomic<bool> s_running{false};
std::thread s_thread;
std::atomic<bool> s_enabled{false}; // Off unless the user turns it on.
std::atomic<bool> s_session{false}; // A game is running, so sharing may start.
std::atomic<int> s_mode{static_cast<int>(Mode::Network)};
// Newest frame id the receiver reports having seen (newer receivers only).
std::atomic<u32> s_acked{0};
std::atomic<bool> s_ack_seen{false};

// A clip on its way to the receiver (PlayClip): at the link's rate, sent as it would play.
std::mutex s_clip_mutex;
std::vector<s16> s_clip;
std::size_t s_clip_sent = 0; // In packets.
Clock::time_point s_clip_start{};

// Settings (GameShare tab).
std::atomic<bool> s_audio_enabled{true};

// Requests from the receiver, applied on the main loop (MergeRemoteInput).
std::atomic<int> s_pending_layout{-1};
std::atomic<int> s_pending_fps{-1};
std::atomic<int> s_host_layout{0}; // What the receiver last asked this console to show.
std::atomic<bool> s_lobby{false};        // The library is on screen: no game to capture.
// Frames the receiver says it has put together, and whether it says so at all.
std::atomic<u32> s_completed{0};
std::atomic<bool> s_completed_seen{false};
// The frame rate the link can take at the moment (lowered while it is congested).
std::atomic<u32> s_fps_cap{60};
std::atomic<bool> s_lobby_wanted{false}; // Sharing was started from the library: keep the link.
std::atomic<bool> s_link_failed{false};  // The thread gave up (no local wireless, no socket).
std::atomic<bool> s_menu_request{false}; // The receiver asked for the in-game menu.

// Whether the game is looking for an Amiibo (set by the Amiibo session), so the
// receiver can offer its own NFC reader.
std::atomic<bool> s_amiibo_wanted{false};
std::mutex s_amiibo_mutex;
bool s_amiibo_pending = false;
AmiiboCapture s_amiibo_capture;

// Sound on its way to the receiver: DSP frames, oldest first.
constexpr std::size_t AUDIO_FRAME_SAMPLES = 160;
constexpr std::size_t MAX_QUEUED_AUDIO = 48; // About a quarter of a second.
std::mutex s_audio_mutex;
std::deque<std::array<s16, AUDIO_FRAME_SAMPLES * 2>> s_audio_queue;
std::atomic<bool> s_audio_streaming{false};

void OnAudioFrame(const s16* samples, std::size_t frames) {
    if (!s_audio_streaming.load(std::memory_order_relaxed) || frames != AUDIO_FRAME_SAMPLES) {
        return;
    }
    std::array<s16, AUDIO_FRAME_SAMPLES * 2> frame;
    std::memcpy(frame.data(), samples, sizeof(frame));
    std::lock_guard lock{s_audio_mutex};
    if (s_audio_queue.size() >= MAX_QUEUED_AUDIO) {
        s_audio_queue.pop_front(); // The receiver is not keeping up: stay current.
    }
    s_audio_queue.push_back(frame);
}

u16 ReadU16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 ReadU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
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

u32 ScreenWidth(u8 screen) {
    if (screen == 3) {
        return 640;
    }
    return screen == 1 ? 320 : 400;
}
// Both screens are sent as the 3DS has them: the top one above, the bottom one centred
// under it, in a 400 x 480 picture.
u32 ScreenHeight(u8 screen) {
    if (screen == 3) {
        return 480;
    }
    return screen == 2 ? 480 : 240;
}

/// BGRA -> JPEG. Returns an empty vector on failure.
std::vector<u8> EncodeJpeg(const std::vector<u8>& bgra, u32 width, u32 height, int quality) {
    std::vector<u8> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0, n = static_cast<std::size_t>(width) * height; i < n; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }

    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char* out = nullptr;
    unsigned long out_size = 0;
    jpeg_mem_dest(&cinfo, &out, &out_size);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(quality, 30, 95), TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = rgb.data() + static_cast<std::size_t>(cinfo.next_scanline) * width * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    std::vector<u8> result;
    if (out != nullptr) {
        result.assign(out, out + out_size);
        std::free(out);
    }
    return result;
}

void HandlePacket(int sock, const u8* data, std::size_t size, const sockaddr_in& from) {
    if (size < HEADER || std::memcmp(data, MAGIC, sizeof(MAGIC)) != 0) {
        return;
    }
    const u8 type = data[4];
    const u8* body = data + HEADER;
    const std::size_t body_size = size - HEADER;
    const auto now = Clock::now();

    if (type == PacketType::Hello && body_size >= 3) {
        u8 screen;
        bool is_new;
        {
            std::lock_guard lock{s.mutex};
            is_new = !s.client_active || s.client_addr.sin_addr.s_addr != from.sin_addr.s_addr ||
                     s.client_addr.sin_port != from.sin_port;
            s.client_active = true;
            s.client_addr = from;
            s.screen = body[0] <= 3 ? body[0] : 1;
            s.audio_wanted = body_size >= 4 && (body[3] & 1) != 0;
            s.quality = body[1];
            s.max_fps = std::clamp<u8>(body[2], 5, 60);
            s.client_seen = now;
            screen = s.screen;
        }
        if (is_new) {
            s_ack_seen.store(false);
            s_completed_seen.store(false);
            LOG_INFO(Frontend, "GameShare: receiver connected from {}:{} ({})",
                     inet_ntoa(from.sin_addr), ntohs(from.sin_port),
                     screen == 2 ? "both screens" : screen == 1 ? "bottom screen" : "top screen");
        }
        u8 reply[HEADER + 5];
        std::memcpy(reply, MAGIC, sizeof(MAGIC));
        reply[4] = PacketType::Welcome;
        WriteU16(reply + HEADER, static_cast<u16>(ScreenWidth(screen)));
        WriteU16(reply + HEADER + 2, static_cast<u16>(ScreenHeight(screen)));
        reply[HEADER + 4] = screen;
        if (sock < 0) {
            GameShareUsb::Send(reply, sizeof(reply));
        } else {
            sendto(sock, reply, sizeof(reply), 0, reinterpret_cast<const sockaddr*>(&from),
                   sizeof(from));
        }
    } else if (type == PacketType::Input && body_size >= 17) {
        bool acted = false;
        {
        std::lock_guard lock{s.mutex};
        if (!s.client_active || s.client_addr.sin_addr.s_addr != from.sin_addr.s_addr) {
            return;
        }
        s.client_seen = now;
        const RemoteInput before = s.input;
        s.input.buttons = ReadU32(body + 4);
        s.input.stick_x = static_cast<s16>(ReadU16(body + 8));
        s.input.stick_y = static_cast<s16>(ReadU16(body + 10));
        s.input.touching = body[12] != 0;
        s.input.touch_x = ReadU16(body + 13);
        s.input.touch_y = ReadU16(body + 15);
        // Newer receivers append the right stick.
        if (body_size >= 21) {
            s.input.right_x = static_cast<s16>(ReadU16(body + 17));
            s.input.right_y = static_cast<s16>(ReadU16(body + 19));
        } else {
            s.input.right_x = s.input.right_y = 0;
        }
        if (body_size >= 25) {
            s_acked.store(ReadU32(body + 21));
            s_ack_seen.store(true);
        }
        s.input.has_motion = body_size >= 50 && body[25] != 0;
        if (body_size >= 52) {
            s_completed.store(ReadU16(body + 50));
            s_completed_seen.store(true);
        }
        if (s.input.has_motion) {
            std::memcpy(s.input.motion, body + 26, sizeof(s.input.motion));
        }
        s.input.when = now;
        constexpr int MOVED = 8000;
        acted = s.input.buttons != before.buttons || s.input.touching ||
                std::abs(s.input.stick_x) > MOVED || std::abs(s.input.stick_y) > MOVED ||
                std::abs(s.input.right_x) > MOVED || std::abs(s.input.right_y) > MOVED;
        }
        // Playing from the other console counts as using this one: without this the
        // system dims the screen, since nobody touches this console's own controls.
        static Clock::time_point last_report{};
        if (acted && now - last_report > std::chrono::seconds{1}) {
            last_report = now;
            ReportUserActivity();
        }
    } else if (type == PacketType::Command && body_size >= 2) {
        // 1: which screen this console should show (0 its own layout, 1 top, 2 bottom).
        // 2: this console's FPS counter (0 off, 1 on).
        if (body[0] == 1 && body[1] <= 2) {
            s_pending_layout.store(body[1]);
        } else if (body[0] == 2) {
            s_pending_fps.store(body[1] != 0 ? 1 : 0);
        } else if (body[0] == 3) {
            s_menu_request.store(true);
        }
    } else if (type == PacketType::Amiibo) {
        // An Amiibo read by the receiver's NFC reader: libnx NfpData, then the UID and
        // the model fields. Anything shorter is not one.
        constexpr std::size_t NFP = 0x298;
        if (body_size >= NFP + 14) {
            std::lock_guard lock{s_amiibo_mutex};
            std::memcpy(s_amiibo_capture.nfp_data.data(), body, NFP);
            std::memcpy(s_amiibo_capture.uid.data(), body + NFP, 7);
            s_amiibo_capture.character_id = ReadU16(body + NFP + 7);
            s_amiibo_capture.character_variant = body[NFP + 9];
            s_amiibo_capture.amiibo_type = body[NFP + 10];
            s_amiibo_capture.model_number = ReadU16(body + NFP + 11);
            s_amiibo_capture.series = body[NFP + 13];
            s_amiibo_capture.name.clear();
            s_amiibo_pending = true;
        }
    }
}

void NetworkThread() {
    Common::SetCurrentThreadName("GameShare");

    // The socket is created after the local network exists, so it lives on it.
    bool local_wireless = false;
    if (s_mode.load() == static_cast<int>(Mode::LocalWireless)) {
        char ldn_log[256];
        local_wireless = StartLocalWirelessHost(ldn_log, sizeof(ldn_log));
        LOG_INFO(Frontend, "GameShare/LDN: {} -> {}", ldn_log,
                 local_wireless ? "network created" : "failed");
    }
    if (s_mode.load() == static_cast<int>(Mode::LocalWireless) && !local_wireless) {
        LOG_ERROR(Frontend, "GameShare: local wireless is not available; not sharing");
        s_link_failed.store(true);
        return;
    }

    const bool usb = s_mode.load() == static_cast<int>(Mode::Usb);
    if (usb) {
        char usb_log[256];
        const bool ok = GameShareUsb::Start(usb_log, sizeof(usb_log));
        LOG_INFO(Frontend, "GameShare/USB: {} -> {}", usb_log,
                 ok ? "waiting for the other console" : "not available");
        if (!ok) {
            LOG_ERROR(Frontend, "GameShare: USB is not available; not sharing");
            return;
        }
    }

    const int sock = usb ? -1 : socket(AF_INET, SOCK_DGRAM, 0);
    if (!usb && sock < 0) {
        LOG_ERROR(Frontend, "GameShare: could not create the socket");
        s_link_failed.store(true);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!usb && bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR(Frontend, "GameShare: could not bind UDP port {}", PORT);
        close(sock);
        if (local_wireless) {
            StopLocalWirelessHost();
        }
        return;
    }
    if (!usb) {
        // Never let frames pile up behind a slow link: with a small, non-blocking send
        // buffer a frame that does not fit is dropped instead of delivered late.
        const int send_buffer = 64 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    }
    LOG_INFO(Frontend, "GameShare: listening on UDP port {} ({})", PORT,
             usb ? "USB" : local_wireless ? "local wireless" : "network");

    std::vector<u8> frame;
    std::vector<u8> packet(HEADER + 17 + CHUNK);
    u8 recv_buffer[1024];
    u32 frame_id = 0;
    auto last_status = Clock::now();
    u32 audio_seq = 0;
    AudioCore::SetOutputTap(OnAudioFrame);

    u32 stat_frames = 0, stat_held = 0;
    // How the link is doing, looked at once a second (see where the frame is encoded).
    u64 stat_held_total = 0, adapt_held = 0;
    u32 adapt_sent = 0;
    u16 adapt_done = 0;
    int adapt_level = 0, adapt_calm = 0, stat_quality = 0;
    auto adapt_checked = Clock::now();
    auto last_sent = Clock::now();
    u64 stat_bytes = 0, stat_encode_us = 0, stat_capture_us = 0;
    auto stat_start = Clock::now();

    while (s_running.load(std::memory_order_relaxed)) {
        if (usb) {
            bool any = false;
            for (int i = 0; i < 16; ++i) {
                const std::size_t got =
                    GameShareUsb::Receive(recv_buffer, sizeof(recv_buffer));
                if (got == 0) {
                    break;
                }
                any = true;
                HandlePacket(-1, recv_buffer, got, sockaddr_in{});
            }
            if (!any) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            // The link's state, whenever it changes (at most every two seconds).
            static Clock::time_point last_status{};
            static Clock::time_point last_logged{};
            static char previous[320];
            if (Clock::now() - last_status > std::chrono::seconds{2}) {
                last_status = Clock::now();
                char status[320];
                GameShareUsb::Status(status, sizeof(status));
                if (std::strncmp(status, previous, 60) != 0 ||
                    Clock::now() - last_logged > std::chrono::seconds{20}) {
                    std::memcpy(previous, status, sizeof(previous));
                    last_logged = Clock::now();
                    LOG_INFO(Frontend, "GameShare/USB: {}", status);
                }
            }
        } else if (pollfd pfd{.fd = sock, .events = POLLIN, .revents = 0};
                   poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN) != 0) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const ssize_t got = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (got > 0) {
                HandlePacket(sock, recv_buffer, static_cast<std::size_t>(got), from);
            }
        }

        // Sound and status go out whether or not there is a picture this pass.
        {
            sockaddr_in target{};
            bool active = false;
            bool audio = false;
            {
                std::lock_guard lock{s.mutex};
                active = s.client_active;
                target = s.client_addr;
                audio = active && s.audio_wanted && s_audio_enabled.load();
            }
            const auto send = [&](const u8* data, std::size_t size) {
                if (usb) {
                    GameShareUsb::Send(data, size);
                } else {
                    sendto(sock, data, size, 0, reinterpret_cast<const sockaddr*>(&target),
                           sizeof(target));
                }
            };
            s_audio_streaming.store(audio, std::memory_order_relaxed);
            if (audio) {
                for (int burst = 0; burst < 8; ++burst) {
                    u8 out[HEADER + 6 + AUDIO_FRAME_SAMPLES * 4];
                    {
                        std::lock_guard lock{s_audio_mutex};
                        if (s_audio_queue.empty()) {
                            break;
                        }
                        std::memcpy(out + HEADER + 6, s_audio_queue.front().data(),
                                    AUDIO_FRAME_SAMPLES * 4);
                        s_audio_queue.pop_front();
                    }
                    std::memcpy(out, MAGIC, sizeof(MAGIC));
                    out[4] = PacketType::Audio;
                    WriteU32(out + HEADER, ++audio_seq);
                    WriteU16(out + HEADER + 4, static_cast<u16>(AUDIO_FRAME_SAMPLES));
                    send(out, sizeof(out));
                }
            } else {
                std::lock_guard lock{s_audio_mutex};
                s_audio_queue.clear();
            }
            {
                // The library's clip: paced by the clock, a little ahead of where it plays.
                std::lock_guard lock{s_clip_mutex};
                if (!s_clip.empty() && (!active || !s_lobby.load())) {
                    s_clip.clear();
                }
                if (!s_clip.empty()) {
                    const std::size_t packets =
                        (s_clip.size() / 2 + AUDIO_FRAME_SAMPLES - 1) / AUDIO_FRAME_SAMPLES;
                    const double elapsed =
                        std::chrono::duration<double>(Clock::now() - s_clip_start).count();
                    const std::size_t due = std::min(
                        packets, static_cast<std::size_t>(elapsed * 32728.0 / AUDIO_FRAME_SAMPLES) + 16);
                    for (int burst = 0; burst < 12 && s_clip_sent < due; ++burst, ++s_clip_sent) {
                        u8 out[HEADER + 6 + AUDIO_FRAME_SAMPLES * 4] = {};
                        const std::size_t first = s_clip_sent * AUDIO_FRAME_SAMPLES * 2;
                        const std::size_t count =
                            std::min(AUDIO_FRAME_SAMPLES * 2, s_clip.size() - first);
                        std::memcpy(out + HEADER + 6, s_clip.data() + first, count * sizeof(s16));
                        std::memcpy(out, MAGIC, sizeof(MAGIC));
                        out[4] = PacketType::Audio;
                        WriteU32(out + HEADER, ++audio_seq);
                        WriteU16(out + HEADER + 4, static_cast<u16>(AUDIO_FRAME_SAMPLES | 0x8000));
                        send(out, sizeof(out));
                    }
                    if (s_clip_sent >= packets) {
                        s_clip.clear();
                    }
                }
            }
            if (active && Clock::now() - last_status > std::chrono::milliseconds{400}) {
                last_status = Clock::now();
                u8 out[HEADER + 3];
                std::memcpy(out, MAGIC, sizeof(MAGIC));
                out[4] = PacketType::Status;
                out[HEADER] = static_cast<u8>((s_amiibo_wanted.load() ? 1 : 0) |
                                              (audio ? 2 : 0) |
                                              (Settings::values.show_fps.GetValue() ? 4 : 0) |
                                              (s_audio_enabled.load() ? 8 : 0) |
                                              (s_lobby.load() ? 16 : 0));
                out[HEADER + 1] = static_cast<u8>(s_host_layout.load());
                // The game's frame rate, for the receiver to show beside the picture.
                const double game_fps =
                    s_lobby.load() ? 0.0
                                   : Core::System::GetInstance().GetLastPerfStats().game_fps;
                out[HEADER + 2] = static_cast<u8>(std::clamp(game_fps + 0.5, 0.0, 255.0));
                send(out, sizeof(out));
            }
        }

        sockaddr_in client{};
        u32 width = 0, height = 0;
        u8 screen = 0;
        int quality = 80;
        u64 capture_us = 0;
        bool have_frame = false;
        {
            std::lock_guard lock{s.mutex};
            if (s.client_active && Clock::now() - s.client_seen > CLIENT_TIMEOUT) {
                s.client_active = false;
                s.input = {};
                LOG_INFO(Frontend, "GameShare: receiver timed out");
            }
            if (s.frame_ready) {
                s.frame_ready = false;
                if (s.client_active) {
                    frame.swap(s.ready);
                    width = s.ready_width;
                    height = s.ready_height;
                    screen = s.ready_screen;
                    quality = s.quality;
                    client = s.client_addr;
                    capture_us = s.capture_micros;
                    have_frame = true;
                }
            }
        }
        if (!have_frame) {
            continue;
        }

        // The receiver reports the newest frame it has seen. When it falls behind, the
        // link is not keeping up: hold frames back instead of queueing them (queued
        // frames are what turns into seconds of delay), but still send one now and then
        // to notice when the link recovers.
        if (s_ack_seen.load() && frame_id - s_acked.load() > MAX_UNACKED_FRAMES &&
            Clock::now() - last_sent < PROBE_INTERVAL) {
            ++stat_held;
            ++stat_held_total;
            continue;
        }

        // Once a second, look at how the link is doing. A frame goes out as some twenty
        // datagrams and is lost if one of them is: on a link losing 5% of its packets
        // only a third of the frames arrive whole. Smaller frames survive, so while the
        // link is struggling the picture gets coarser and a little slower instead of
        // late, and comes back when the link does.
        if (const auto now = Clock::now(); now - adapt_checked >= std::chrono::seconds{1}) {
            adapt_checked = now;
            const u32 held_now = static_cast<u32>(stat_held_total - adapt_held);
            adapt_held = stat_held_total;
            bool struggling = held_now >= 4;
            if (s_completed_seen.load()) {
                const u16 done = static_cast<u16>(s_completed.load());
                const u32 arrived = static_cast<u16>(done - adapt_done);
                const u32 sent = frame_id - adapt_sent;
                // What was sent in the last moments may still be on its way.
                struggling = struggling || (sent >= 10 && arrived + 3 < sent * 85 / 100);
                adapt_done = done;
            }
            adapt_sent = frame_id;
            if (struggling) {
                adapt_level = std::min(adapt_level + 1, 3);
                adapt_calm = 0;
            } else if (adapt_level > 0 && ++adapt_calm >= 4) {
                --adapt_level;
                adapt_calm = 0;
            }
            s_fps_cap.store(adapt_level >= 2 ? 20 : 60);
        }
        static constexpr int quality_step[4] = {0, 22, 38, 50};
        quality = std::max(28, quality - quality_step[adapt_level]);
        stat_quality = quality;

        const auto encode_start = Clock::now();
        const std::vector<u8> jpeg = EncodeJpeg(frame, width, height, quality);
        if (jpeg.empty()) {
            continue;
        }
        const u64 encode_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - encode_start)
                .count());

        ++frame_id;
        const u16 chunk_count = static_cast<u16>((jpeg.size() + CHUNK - 1) / CHUNK);
        for (u16 index = 0; index < chunk_count; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * CHUNK;
            const std::size_t size = std::min(CHUNK, jpeg.size() - offset);
            u8* p = packet.data();
            std::memcpy(p, MAGIC, sizeof(MAGIC));
            p[4] = PacketType::Frame;
            WriteU32(p + HEADER, frame_id);
            WriteU16(p + HEADER + 4, index);
            WriteU16(p + HEADER + 6, chunk_count);
            WriteU32(p + HEADER + 8, static_cast<u32>(jpeg.size()));
            WriteU16(p + HEADER + 12, static_cast<u16>(width));
            WriteU16(p + HEADER + 14, static_cast<u16>(height));
            p[HEADER + 16] = screen;
            std::memcpy(p + HEADER + 17, jpeg.data() + offset, size);
            const std::size_t length = HEADER + 17 + size;
            // A full queue drops the rest of this frame: late is worse than missing.
            if (usb) {
                if (!GameShareUsb::Send(p, length)) {
                    break;
                }
            } else if (sendto(sock, p, length, 0, reinterpret_cast<const sockaddr*>(&client),
                              sizeof(client)) < 0) {
                break;
            }
        }
        last_sent = Clock::now();

        ++stat_frames;
        stat_bytes += jpeg.size();
        stat_encode_us += encode_us;
        stat_capture_us += capture_us;
        if (const auto now = Clock::now(); now - stat_start >= STATS_INTERVAL) {
            const double seconds = std::chrono::duration<double>(now - stat_start).count();
            LOG_INFO(Frontend,
                     "GameShare: {:.1f} fps, {} KB/s, jpeg {} bytes avg, capture {:.1f} ms avg, "
                     "encode {:.1f} ms avg, {} frames held back, quality {} (link level {})",
                     stat_frames / seconds, static_cast<u64>(stat_bytes / seconds / 1024),
                     stat_bytes / stat_frames, stat_capture_us / 1000.0 / stat_frames,
                     stat_encode_us / 1000.0 / stat_frames, stat_held, stat_quality,
                     adapt_level);
            stat_frames = stat_held = 0;
            stat_bytes = stat_encode_us = stat_capture_us = 0;
            stat_start = now;
        }
    }

    AudioCore::SetOutputTap(nullptr);
    s_audio_streaming.store(false);
    if (sock >= 0) {
        close(sock);
    }
    if (usb) {
        GameShareUsb::Stop();
    }
    if (local_wireless) {
        StopLocalWirelessHost();
    }
}

} // namespace

bool IsEnabled() {
    return s_enabled.load();
}

void SetEnabled(bool enabled) {
    s_enabled.store(enabled);
    if (!enabled) {
        // Off is off everywhere, at once: in a game, and a link left waiting in the
        // library.
        const bool session = s_session.load();
        s_lobby_wanted.store(false);
        s_lobby.store(false);
        Stop();
        s_session.store(session);
        return;
    }
    // Mid-game (the in-game menu's switch): takes effect at once.
    if (s_session.load()) {
        Start();
    }
}

const char* ModeName() {
    switch (static_cast<Mode>(s_mode.load())) {
    case Mode::LocalWireless:
        return "local wireless";
    case Mode::Usb:
        return "USB";
    default:
        return "Wi-Fi / LAN";
    }
}

int GetMode() {
    return s_mode.load();
}

bool IsAudioEnabled() {
    return s_audio_enabled.load();
}

void SetAudioEnabled(bool enabled) {
    s_audio_enabled.store(enabled);
}

void SetAmiiboWanted(bool wanted) {
    s_amiibo_wanted.store(wanted);
}

bool TakeRemoteAmiibo(AmiiboCapture& out) {
    std::lock_guard lock{s_amiibo_mutex};
    if (!s_amiibo_pending) {
        return false;
    }
    out = s_amiibo_capture;
    s_amiibo_pending = false;
    return true;
}

bool HasReceiver() {
    if (!s_running.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock{s.mutex};
    return s.client_active;
}

void SetMode(int mode) {
    // Mode::Usb exists but is not selectable until the loader grants the USB services.
    s_mode.store(std::clamp(mode, 0, static_cast<int>(Mode::LocalWireless)));
}

void Start() {
    s_session.store(true);
    s_lobby.store(false);
    if (!s_enabled.load()) {
        // Switched off between the library and the game (the notice before a game lets
        // one do that): a link made in the library goes with it.
        if (s_running.load()) {
            s_lobby_wanted.store(false);
            Stop();
            s_session.store(true);
        }
        return;
    }
    if (s_running.exchange(true)) {
        return;
    }
    if (s_thread.joinable()) {
        s_thread.join(); // One that gave up earlier.
    }
    s_link_failed.store(false);
    {
        std::lock_guard lock{s.mutex};
        s.client_active = false;
        s.capture_pending = false;
        s.frame_ready = false;
        s.input = {};
    }
    s_thread = std::thread{NetworkThread};
}

void StartLobby() {
    s_lobby_wanted.store(true);
    s_enabled.store(true);
    if (s_link_failed.load()) {
        // The last attempt gave up; this is a new one.
        s_running.store(false);
    }
    Start();
    s_session.store(false);
    s_lobby.store(true);
}

void PlayClip(const short* stereo, std::size_t frames, int rate) {
    if (stereo == nullptr || frames < 2 || rate < 4000 || !s_lobby.load()) {
        return;
    }
    frames = std::min<std::size_t>(frames, static_cast<std::size_t>(rate) * 20);
    const double step = static_cast<double>(rate) / 32728.0;
    const std::size_t out_frames = static_cast<std::size_t>(static_cast<double>(frames - 1) / step);
    std::vector<s16> clip(out_frames * 2);
    for (std::size_t i = 0; i < out_frames; ++i) {
        const double at = static_cast<double>(i) * step;
        const std::size_t k = static_cast<std::size_t>(at);
        const double f = at - static_cast<double>(k);
        for (int ch = 0; ch < 2; ++ch) {
            const double a = stereo[k * 2 + ch], b = stereo[(k + 1) * 2 + ch];
            clip[i * 2 + ch] = static_cast<s16>((a + (b - a) * f) * 0.7);
        }
    }
    std::lock_guard lock{s_clip_mutex};
    s_clip = std::move(clip);
    s_clip_sent = 0;
    s_clip_start = Clock::now();
}

void StopClip() {
    std::lock_guard lock{s_clip_mutex};
    s_clip.clear();
}

void StopLobby() {
    s_lobby_wanted.store(false);
    s_lobby.store(false);
    s_enabled.store(false);
    Stop();
}

bool IsLobbyOn() {
    return s_lobby_wanted.load();
}

int LinkState() {
    if (!s_running.load()) {
        return 0;
    }
    if (s_link_failed.load()) {
        return 3;
    }
    std::lock_guard lock{s.mutex};
    return s.client_active ? 2 : 1;
}

bool LobbyFrameDue() {
    if (!s_running.load(std::memory_order_relaxed) || !s_lobby.load()) {
        return false;
    }
    std::lock_guard lock{s.mutex};
    if (!s.client_active || s.frame_ready || s.screen != 3) {
        return false;
    }
    // Menus do not need a game's frame rate, and a still one costs the link nothing.
    return Clock::now() - s.last_capture >= std::chrono::milliseconds{66};
}

void SubmitLobbyFrame(const unsigned int* pixels, int width, int height) {
    std::lock_guard lock{s.mutex};
    const std::size_t count = static_cast<std::size_t>(width) * height;
    s.ready.resize(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned int p = pixels[i]; // 0xAABBGGRR -> B G R A
        s.ready[i * 4 + 0] = static_cast<u8>(p >> 16);
        s.ready[i * 4 + 1] = static_cast<u8>(p >> 8);
        s.ready[i * 4 + 2] = static_cast<u8>(p);
        s.ready[i * 4 + 3] = 0xFF;
    }
    s.ready_width = static_cast<u32>(width);
    s.ready_height = static_cast<u32>(height);
    s.ready_screen = 3;
    s.capture_micros = 0;
    s.last_capture = Clock::now();
    s.frame_ready = true;
}

bool GetRemotePad(RemotePad& out) {
    if (!s_running.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock{s.mutex};
    if (!s.client_active || Clock::now() - s.input.when > INPUT_TIMEOUT) {
        return false;
    }
    out.buttons = s.input.buttons;
    out.stick_x = s.input.stick_x;
    out.stick_y = s.input.stick_y;
    out.right_x = s.input.right_x;
    out.right_y = s.input.right_y;
    out.touching = s.input.touching;
    out.touch_x = s.input.touch_x;
    out.touch_y = s.input.touch_y;
    return true;
}

bool TakeMenuRequest() {
    return s_menu_request.exchange(false);
}

void Stop() {
    s_session.store(false);
    // A link that is up when a game ends goes back to the library with it, whether it
    // was made there or in the game: the other console then shows the games, not the
    // last picture of the one that closed.
    if (s_enabled.load() && s_running.load() && !s_link_failed.load()) {
        s_lobby_wanted.store(true);
    }
    if (s_lobby_wanted.load()) {
        // The link was made from the library: the game ending hands it back there.
        s_lobby.store(true);
        s_pending_layout.store(-1);
        s_host_layout.store(0);
        std::lock_guard lock{s.mutex};
        s.capture_pending = false;
        s.frame_ready = false;
        return;
    }
    if (!s_running.exchange(false)) {
        return;
    }
    if (s_thread.joinable()) {
        s_thread.join();
    }
    s_pending_layout.store(-1);
    s_host_layout.store(0);
    std::lock_guard lock{s.mutex};
    s.client_active = false;
    s.input = {};
}

void Tick(Core::System& system) {
    if (!s_running.load(std::memory_order_relaxed)) {
        return;
    }

    u8 screen;
    {
        std::lock_guard lock{s.mutex};
        const auto now = Clock::now();
        if (!s.client_active || s.capture_pending) {
            return;
        }
        const u32 fps = std::clamp<u32>(std::min<u32>(s.max_fps, s_fps_cap.load()), 5, 60);
        const auto interval = std::chrono::microseconds{1'000'000 / fps};
        // The capture is taken at the next presented frame. Asking a quarter of an
        // interval early makes it the frame that is due, not the one after it, which
        // used to add about half a frame of latency.
        const auto due = s.last_capture + interval;
        if (now < due - interval / 4) {
            return;
        }
        screen = s.screen == 3 ? 1 : s.screen;
        s.capture_pending = true;
        s.capture_requested = now;
        // An early capture still counts from when it was due: counting from `now` let a
        // game presenting at 60 be sent at 40 frames a second when 30 were asked for -
        // a third more data than the link had been sized for.
        s.last_capture = now < due ? due : now;
    }

    auto& renderer = system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        std::lock_guard lock{s.mutex};
        s.capture_pending = false;
        return;
    }

    const u32 width = ScreenWidth(screen);
    const u32 height = ScreenHeight(screen);
    {
        std::lock_guard lock{s.mutex};
        s.capture.resize(static_cast<std::size_t>(width) * height * 4);
    }
    // One screen, at the 3DS's own resolution, whatever the host itself is showing.
    const Layout::FramebufferLayout layout =
        screen == 2 ? Layout::DefaultFrameLayout(width, height, false, false)
                    : Layout::SingleFrameLayout(width, height, screen == 1, false);
    renderer.RequestScreenshot(
        s.capture.data(),
        [width, height, screen](bool) {
            // Runs on the render thread once the pixels are in s.capture.
            std::lock_guard lock{s.mutex};
            s.ready = s.capture;
            s.ready_width = width;
            s.ready_height = height;
            s.ready_screen = screen;
            s.capture_micros =
                static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     Clock::now() - s.capture_requested)
                                     .count());
            s.frame_ready = true;
            s.capture_pending = false;
        },
        layout);
}

void MergeRemoteInput(InputState& state) {
    if (!s_running.load(std::memory_order_relaxed) || s_lobby.load()) {
        return;
    }
    // What the receiver asked this console to do.
    if (const int layout = s_pending_layout.exchange(-1); layout >= 0) {
        s_host_layout.store(layout);
        SetSharedScreenLayout(layout);
    }
    if (const int fps = s_pending_fps.exchange(-1); fps >= 0) {
        Settings::values.show_fps = fps != 0;
    }
    RemoteInput input;
    {
        std::lock_guard lock{s.mutex};
        if (!s.client_active || Clock::now() - s.input.when > INPUT_TIMEOUT) {
            return;
        }
        input = s.input;
    }

    state.buttons |= input.buttons;

    // The receiver's stick takes over only while the local one is at rest.
    constexpr int DEADZONE = 6000;
    if (std::abs(state.left_x) < DEADZONE && std::abs(state.left_y) < DEADZONE &&
        (std::abs(input.stick_x) >= DEADZONE || std::abs(input.stick_y) >= DEADZONE)) {
        state.left_x = input.stick_x;
        state.left_y = input.stick_y;
    }
    if (std::abs(state.right_x) < DEADZONE && std::abs(state.right_y) < DEADZONE &&
        (std::abs(input.right_x) >= DEADZONE || std::abs(input.right_y) >= DEADZONE)) {
        state.right_x = input.right_x;
        state.right_y = input.right_y;
    }

    // Motion: whichever console is being moved is the one that aims. Sticky, so the
    // source does not flip back while the hands are still for a moment.
    if (input.has_motion) {
        const auto magnitude = [](float x, float y, float z) {
            return std::sqrt(x * x + y * y + z * z);
        };
        constexpr float MOVING = 0.05f;
        const float remote = magnitude(input.motion[3], input.motion[4], input.motion[5]);
        const float local =
            state.motion.active
                ? magnitude(state.motion.gyro_x, state.motion.gyro_y, state.motion.gyro_z)
                : 0.0f;
        static bool use_remote = false;
        if (remote > MOVING && remote > local) {
            use_remote = true;
        } else if (local > MOVING && local > remote) {
            use_remote = false;
        }
        if (use_remote || !state.motion.active) {
            state.motion = {
                .active = true,
                .accel_x = input.motion[0],
                .accel_y = input.motion[1],
                .accel_z = input.motion[2],
                .gyro_x = input.motion[3],
                .gyro_y = input.motion[4],
                .gyro_z = input.motion[5],
            };
        }
    }

    // Touch arrives as a position on the bottom screen and is applied there directly, so
    // it works whatever this console is showing - the top screen alone, for instance.
    if (input.touching && !state.touch_pressed) {
        state.touch_on_screen = true;
        state.touch_fx = (std::min<u32>(input.touch_x, 319) + 0.5f) / 320.0f;
        state.touch_fy = (std::min<u32>(input.touch_y, 239) + 0.5f) / 240.0f;
    }
}

} // namespace SwitchFrontend::GameShare
