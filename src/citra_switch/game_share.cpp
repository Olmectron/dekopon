// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "citra_switch/emu_window.h"
#include "citra_switch/game_share.h"
#include "citra_switch/game_share_ldn.h"
#include "citra_switch/game_share_usb.h"
#include "citra_switch/input.h"
#include "common/common_types.h"
#include "common/logging/log.h"
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
    u8 screen{1};
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
std::atomic<bool> s_enabled{true};
std::atomic<int> s_mode{static_cast<int>(Mode::Network)};
// Newest frame id the receiver reports having seen (newer receivers only).
std::atomic<u32> s_acked{0};
std::atomic<bool> s_ack_seen{false};

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
    return screen == 1 ? 320 : 400;
}
constexpr u32 SCREEN_HEIGHT = 240;

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
            s.screen = body[0] == 0 ? 0 : 1;
            s.quality = body[1];
            s.max_fps = std::clamp<u8>(body[2], 5, 60);
            s.client_seen = now;
            screen = s.screen;
        }
        if (is_new) {
            s_ack_seen.store(false);
            LOG_INFO(Frontend, "GameShare: receiver connected from {}:{} ({} screen)",
                     inet_ntoa(from.sin_addr), ntohs(from.sin_port), screen == 1 ? "bottom" : "top");
        }
        u8 reply[HEADER + 5];
        std::memcpy(reply, MAGIC, sizeof(MAGIC));
        reply[4] = PacketType::Welcome;
        WriteU16(reply + HEADER, static_cast<u16>(ScreenWidth(screen)));
        WriteU16(reply + HEADER + 2, static_cast<u16>(SCREEN_HEIGHT));
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
    u8 recv_buffer[256];
    u32 frame_id = 0;

    u32 stat_frames = 0, stat_held = 0;
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
            continue;
        }

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
                     "encode {:.1f} ms avg, {} frames held back",
                     stat_frames / seconds, static_cast<u64>(stat_bytes / seconds / 1024),
                     stat_bytes / stat_frames, stat_capture_us / 1000.0 / stat_frames,
                     stat_encode_us / 1000.0 / stat_frames, stat_held);
            stat_frames = stat_held = 0;
            stat_bytes = stat_encode_us = stat_capture_us = 0;
            stat_start = now;
        }
    }

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
}

int GetMode() {
    return s_mode.load();
}

void SetMode(int mode) {
    s_mode.store(std::clamp(mode, 0, static_cast<int>(Mode::Count) - 1));
}

void Start() {
    if (!s_enabled.load()) {
        return;
    }
    if (s_running.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock{s.mutex};
        s.client_active = false;
        s.capture_pending = false;
        s.frame_ready = false;
        s.input = {};
    }
    s_thread = std::thread{NetworkThread};
}

void Stop() {
    if (!s_running.exchange(false)) {
        return;
    }
    if (s_thread.joinable()) {
        s_thread.join();
    }
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
        const auto interval = std::chrono::microseconds{1'000'000 / std::max<u32>(s.max_fps, 1)};
        // The capture is taken at the next presented frame. Asking a quarter of an
        // interval early makes it the frame that is due, not the one after it, which
        // used to add about half a frame of latency.
        if (now - s.last_capture < interval - interval / 4) {
            return;
        }
        screen = s.screen;
        s.capture_pending = true;
        s.capture_requested = now;
        s.last_capture = now;
    }

    auto& renderer = system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        std::lock_guard lock{s.mutex};
        s.capture_pending = false;
        return;
    }

    const u32 width = ScreenWidth(screen);
    const u32 height = SCREEN_HEIGHT;
    {
        std::lock_guard lock{s.mutex};
        s.capture.resize(static_cast<std::size_t>(width) * height * 4);
    }
    // One screen, at the 3DS's own resolution, whatever the host itself is showing.
    const Layout::FramebufferLayout layout =
        Layout::SingleFrameLayout(width, height, screen == 1, false);
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
    if (!s_running.load(std::memory_order_relaxed)) {
        return;
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

    // Touch arrives in bottom-screen pixels; the emulator window wants window coordinates, so
    // it only lands when the host's own layout shows the bottom screen (prototype limitation).
    if (input.touching && !state.touch_pressed) {
        if (EmuWindow_Switch* window = GetEmuWindow()) {
            const auto& bottom = window->GetFramebufferLayout().bottom_screen;
            if (bottom.GetWidth() > 0 && bottom.GetHeight() > 0) {
                state.touch_pressed = true;
                state.touch_x =
                    bottom.left + std::min<u32>(input.touch_x, 319) * bottom.GetWidth() / 320;
                state.touch_y =
                    bottom.top + std::min<u32>(input.touch_y, 239) * bottom.GetHeight() / 240;
            }
        }
    }
}

} // namespace SwitchFrontend::GameShare
