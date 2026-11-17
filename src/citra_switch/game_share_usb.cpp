// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GameShare over USB. Its own translation unit: <switch.h> and the emulator's headers do not
// mix. Also compiled into Dekopon Link (tools/gameshare/receiver-nx).
//
// The link is a byte stream (bulk in + bulk out). Datagrams are framed as
//   0xD5 0x4B, u16 length (little endian), payload
// and a transfer is never an exact multiple of the packet size, so the other end's read
// always completes on a short packet instead of waiting for more.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <switch.h>

#include "game_share_usb.h"

namespace GameShareUsb {
namespace {

constexpr std::size_t XFER = 0x4000;
constexpr std::size_t FIFO_SIZE = 512 * 1024;
constexpr std::size_t MAX_DATAGRAM = 2048;
constexpr u8 FRAME_MAGIC0 = 0xD5;
constexpr u8 FRAME_MAGIC1 = 0x4B;
constexpr u16 COMMS_VENDOR = 0x057E;
constexpr u16 COMMS_PRODUCT = 0x3000;

enum class LinkRole { None, Device, Host };

struct Fifo {
    u8* data{};
    std::size_t head{};
    std::size_t size{};

    void Clear() {
        head = size = 0;
    }
    std::size_t Free() const {
        return FIFO_SIZE - size;
    }
    void Push(const u8* src, std::size_t n) {
        std::size_t tail = (head + size) % FIFO_SIZE;
        const std::size_t first = n < FIFO_SIZE - tail ? n : FIFO_SIZE - tail;
        std::memcpy(data + tail, src, first);
        std::memcpy(data, src + first, n - first);
        size += n;
    }
    void Peek(std::size_t offset, u8* dst, std::size_t n) const {
        const std::size_t at = (head + offset) % FIFO_SIZE;
        const std::size_t first = n < FIFO_SIZE - at ? n : FIFO_SIZE - at;
        std::memcpy(dst, data + at, first);
        std::memcpy(dst + first, data, n - first);
    }
    void Drop(std::size_t n) {
        head = (head + n) % FIFO_SIZE;
        size -= n;
    }
};

Mutex s_mutex;
Fifo s_tx;
Fifo s_rx;
Thread s_thread;
bool s_thread_started = false;
volatile bool s_running = false;
volatile LinkRole s_role = LinkRole::None;
bool s_device_ok = false;
bool s_host_ok = false;

alignas(0x1000) u8 s_read_buffer[XFER];
alignas(0x1000) u8 s_write_buffer[XFER];

// Host role state.
UsbHsClientIfSession s_if{};
UsbHsClientEpSession s_ep_in{};
UsbHsClientEpSession s_ep_out{};

// Transfers in flight.
bool s_read_pending = false;
bool s_write_pending = false;
u32 s_read_urb = 0;
u32 s_write_urb = 0;

void Append(char* log, std::size_t log_size, const char* format, unsigned value) {
    const std::size_t used = std::strlen(log);
    if (used + 1 < log_size) {
        std::snprintf(log + used, log_size - used, format, value);
    }
}

bool DeviceConfigured() {
    UsbState state{};
    return R_SUCCEEDED(usbDsGetState(&state)) && static_cast<u32>(state) == 5; // configured
}

/// Host role: finds the other console's usbComms interface and opens its two bulk endpoints.
bool HostAcquire() {
    UsbHsInterfaceFilter filter{};
    filter.Flags = UsbHsInterfaceFilterFlags_idVendor | UsbHsInterfaceFilterFlags_idProduct |
                   UsbHsInterfaceFilterFlags_bInterfaceClass;
    filter.idVendor = COMMS_VENDOR;
    filter.idProduct = COMMS_PRODUCT;
    filter.bInterfaceClass = 0xFF;

    static UsbHsInterface interfaces[4];
    s32 total = 0;
    if (R_FAILED(usbHsQueryAvailableInterfaces(&filter, interfaces, sizeof(interfaces), &total)) ||
        total <= 0) {
        return false;
    }
    if (R_FAILED(usbHsAcquireUsbIf(&s_if, &interfaces[0]))) {
        return false;
    }

    usb_endpoint_descriptor* in = nullptr;
    usb_endpoint_descriptor* out = nullptr;
    for (int i = 0; i < 15; ++i) {
        usb_endpoint_descriptor* d = &s_if.inf.inf.input_endpoint_descs[i];
        if (in == nullptr && d->bLength != 0 && (d->bmAttributes & 3) == 2) {
            in = d;
        }
        d = &s_if.inf.inf.output_endpoint_descs[i];
        if (out == nullptr && d->bLength != 0 && (d->bmAttributes & 3) == 2) {
            out = d;
        }
    }
    if (in == nullptr || out == nullptr ||
        R_FAILED(usbHsIfOpenUsbEp(&s_if, &s_ep_in, 1, XFER, in))) {
        usbHsIfClose(&s_if);
        return false;
    }
    if (R_FAILED(usbHsIfOpenUsbEp(&s_if, &s_ep_out, 1, XFER, out))) {
        usbHsEpClose(&s_ep_in);
        usbHsIfClose(&s_if);
        return false;
    }
    return true;
}

void HostRelease() {
    usbHsEpClose(&s_ep_in);
    usbHsEpClose(&s_ep_out);
    usbHsIfClose(&s_if);
}

Event* ReadEvent() {
    return s_role == LinkRole::Device ? usbCommsGetReadCompletionEvent(0)
                                      : usbHsEpGetXferEvent(&s_ep_in);
}
Event* WriteEvent() {
    return s_role == LinkRole::Device ? usbCommsGetWriteCompletionEvent(0)
                                      : usbHsEpGetXferEvent(&s_ep_out);
}

bool PostRead() {
    if (s_role == LinkRole::Device) {
        return R_SUCCEEDED(usbCommsReadAsync(s_read_buffer, XFER, &s_read_urb, 0));
    }
    return R_SUCCEEDED(usbHsEpPostBufferAsync(&s_ep_in, s_read_buffer, XFER, 0, &s_read_urb));
}

bool PostWrite(std::size_t size) {
    if (s_role == LinkRole::Device) {
        return R_SUCCEEDED(usbCommsWriteAsync(s_write_buffer, size, &s_write_urb, 0));
    }
    return R_SUCCEEDED(usbHsEpPostBufferAsync(&s_ep_out, s_write_buffer,
                                              static_cast<u32>(size), 0, &s_write_urb));
}

/// Result of a finished transfer; false when it failed.
bool Finish(bool read, u32& transferred) {
    transferred = 0;
    if (s_role == LinkRole::Device) {
        return R_SUCCEEDED(read ? usbCommsGetReadResult(s_read_urb, &transferred, 0)
                                : usbCommsGetWriteResult(s_write_urb, &transferred, 0));
    }
    UsbHsXferReport report{};
    u32 count = 0;
    if (R_FAILED(usbHsEpGetXferReport(read ? &s_ep_in : &s_ep_out, &report, 1, &count)) ||
        count < 1 || R_FAILED(report.res)) {
        return false;
    }
    transferred = report.transferredSize;
    return true;
}

/// One step of the link: keeps a read posted, sends what is queued. False when the link broke.
bool Pump() {
    if (!s_read_pending) {
        if (!PostRead()) {
            return false;
        }
        s_read_pending = true;
    }
    if (!s_write_pending) {
        std::size_t take = 0;
        mutexLock(&s_mutex);
        take = s_tx.size < XFER ? s_tx.size : XFER;
        if (take > 1 && take % 64 == 0) {
            --take; // never a whole number of packets: see the top of the file
        }
        if (take > 0) {
            s_tx.Peek(0, s_write_buffer, take);
            s_tx.Drop(take);
        }
        mutexUnlock(&s_mutex);
        if (take > 0) {
            if (!PostWrite(take)) {
                return false;
            }
            s_write_pending = true;
        }
    }

    s32 index = -1;
    const Result rc = waitMulti(&index, 1'000'000, waiterForEvent(ReadEvent()),
                                waiterForEvent(WriteEvent()));
    if (R_FAILED(rc)) {
        return true; // timed out: nothing finished
    }
    if (index == 0) {
        eventClear(ReadEvent());
        if (!s_read_pending) {
            return true;
        }
        u32 got = 0;
        if (!Finish(true, got)) {
            return false;
        }
        s_read_pending = false;
        mutexLock(&s_mutex);
        if (got > 0 && got <= XFER && s_rx.Free() >= got) {
            s_rx.Push(s_read_buffer, got);
        }
        mutexUnlock(&s_mutex);
    } else if (index == 1) {
        eventClear(WriteEvent());
        if (!s_write_pending) {
            return true;
        }
        u32 sent = 0;
        if (!Finish(false, sent)) {
            return false;
        }
        s_write_pending = false;
    }
    return true;
}

void IoThread(void*) {
    u32 ticks = 0;
    while (s_running) {
        if (s_role == LinkRole::None) {
            LinkRole found = LinkRole::None;
            if (s_device_ok && DeviceConfigured()) {
                found = LinkRole::Device;
            } else if (s_host_ok && HostAcquire()) {
                found = LinkRole::Host;
            }
            if (found == LinkRole::None) {
                svcSleepThread(100'000'000);
                continue;
            }
            mutexLock(&s_mutex);
            s_tx.Clear();
            s_rx.Clear();
            mutexUnlock(&s_mutex);
            s_read_pending = s_write_pending = false;
            s_role = found;
        }

        bool alive = Pump();
        // A pulled cable never completes the pending read: ask the stack now and then.
        if (alive && s_role == LinkRole::Device && (++ticks % 256) == 0) {
            alive = DeviceConfigured();
        }
        if (!alive) {
            if (s_role == LinkRole::Host) {
                HostRelease();
            }
            s_role = LinkRole::None;
            svcSleepThread(200'000'000);
        }
    }
    if (s_role == LinkRole::Host) {
        HostRelease();
    }
    s_role = LinkRole::None;
}

} // namespace

bool Start(char* log, std::size_t log_size) {
    if (log_size > 0) {
        log[0] = '\0';
    }
    if (s_running) {
        return true;
    }
    mutexInit(&s_mutex);
    if (s_tx.data == nullptr) {
        s_tx.data = static_cast<u8*>(std::malloc(FIFO_SIZE));
        s_rx.data = static_cast<u8*>(std::malloc(FIFO_SIZE));
    }
    if (s_tx.data == nullptr || s_rx.data == nullptr) {
        return false;
    }
    s_tx.Clear();
    s_rx.Clear();

    Result rc = usbCommsInitialize();
    Append(log, log_size, "usbCommsInitialize(device)=0x%x ", rc);
    s_device_ok = R_SUCCEEDED(rc);
    rc = usbHsInitialize();
    Append(log, log_size, "usbHsInitialize(host)=0x%x ", rc);
    s_host_ok = R_SUCCEEDED(rc);
    if (!s_device_ok && !s_host_ok) {
        return false;
    }

    s_role = LinkRole::None;
    s_running = true;
    rc = threadCreate(&s_thread, IoThread, nullptr, nullptr, 0x10000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&s_thread);
        if (R_FAILED(rc)) {
            threadClose(&s_thread);
        }
    }
    Append(log, log_size, "thread=0x%x", rc);
    s_thread_started = R_SUCCEEDED(rc);
    if (!s_thread_started) {
        s_running = false;
        Stop();
        return false;
    }
    return true;
}

void Stop() {
    s_running = false;
    if (s_thread_started) {
        threadWaitForExit(&s_thread);
        threadClose(&s_thread);
        s_thread_started = false;
    }
    if (s_device_ok) {
        usbCommsExit();
        s_device_ok = false;
    }
    if (s_host_ok) {
        usbHsExit();
        s_host_ok = false;
    }
}

bool IsConnected() {
    return s_role != LinkRole::None;
}

const char* Role() {
    switch (s_role) {
    case LinkRole::Device:
        return "device";
    case LinkRole::Host:
        return "host";
    default:
        return "";
    }
}

bool Send(const void* data, std::size_t size) {
    if (s_role == LinkRole::None || size == 0 || size > MAX_DATAGRAM) {
        return false;
    }
    const u8 header[4] = {FRAME_MAGIC0, FRAME_MAGIC1, static_cast<u8>(size),
                          static_cast<u8>(size >> 8)};
    mutexLock(&s_mutex);
    const bool fits = s_tx.Free() >= size + sizeof(header);
    if (fits) {
        s_tx.Push(header, sizeof(header));
        s_tx.Push(static_cast<const u8*>(data), size);
    }
    mutexUnlock(&s_mutex);
    return fits;
}

std::size_t Receive(void* buffer, std::size_t capacity) {
    std::size_t result = 0;
    mutexLock(&s_mutex);
    while (s_rx.size >= 4) {
        u8 header[4];
        s_rx.Peek(0, header, sizeof(header));
        const std::size_t length = header[2] | (static_cast<std::size_t>(header[3]) << 8);
        if (header[0] != FRAME_MAGIC0 || header[1] != FRAME_MAGIC1 || length == 0 ||
            length > MAX_DATAGRAM) {
            s_rx.Drop(1); // out of step: look for the next frame start
            continue;
        }
        if (s_rx.size < 4 + length) {
            break; // the rest has not arrived yet
        }
        if (length <= capacity) {
            s_rx.Peek(4, static_cast<u8*>(buffer), length);
            result = length;
        }
        s_rx.Drop(4 + length);
        if (result != 0) {
            break;
        }
    }
    mutexUnlock(&s_mutex);
    return result;
}

} // namespace GameShareUsb
