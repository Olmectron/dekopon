// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <switch.h>

#include "citra_switch/amiibo_session.h"
#include "citra_switch/applets/swkbd.h"
#include "citra_switch/updater.h"
#include "citra_switch/config.h"
#include "citra_switch/game_share.h"
#include "citra_switch/input.h"
#include "citra_switch/menu.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/usb_storage.h"
#include "common/horizon_thread.h"
#include "common/stall_watch.h"

namespace Common {
void StopAllThreadWorkers();
}

extern "C" {
#if defined(__SWITCH2__)
// Switch 2 bring-up: use whatever applet type kaiHEN launched us with instead of
// forcing Application (title takeover). Forcing Application aborts in appInit —
// before main, before any SD folder is created — when the loader did not grant
// it, which matches dekopon exiting instantly under kaiHEN/OunceShell.
u32 __nx_applet_type = AppletType_Default;
// Defined by externals/switch_stubs/oaknut/horizon_jit.cpp.
extern unsigned int g_oaknut_horizon_jit_last_rc;
extern unsigned int g_oaknut_horizon_jit_failures;
#else
u32 __nx_applet_type = AppletType_Application;
#endif
size_t __nx_heap_size = 0;
}

// The app's folder was sdmc:/switch/dekopon while this was Dekopon's port. It is
// sdmc:/switch/toronja now; an existing folder is renamed, once, before anything is
// opened in it - saves, settings, caches and games inside it come along untouched.
// (Dekopon itself, if it is still on the card, starts afresh after that.)
static void MoveOldUserFolder(FsFileSystem* sdmc) {
    static bool done = false;
    if (done || sdmc == nullptr) {
        return;
    }
    done = true;
    FsDir dir;
    if (R_SUCCEEDED(fsFsOpenDirectory(sdmc, "/switch/toronja", FsDirOpenMode_ReadDirs, &dir))) {
        fsDirClose(&dir);
        return;
    }
    if (R_SUCCEEDED(fsFsOpenDirectory(sdmc, "/switch/dekopon", FsDirOpenMode_ReadDirs, &dir))) {
        fsDirClose(&dir);
        fsFsRenameDirectory(sdmc, "/switch/dekopon", "/switch/toronja");
    }
}

#if !defined(__SWITCH2__)
// libnx calls this at the end of its own start-up, before any global constructor runs -
// and so before anything can create the new folder and leave the old one behind. (The
// Switch 2 build has its own userAppInit below, which gets there through its boot log.)
extern "C" void userAppInit(void) {
    MoveOldUserFolder(fsdevGetDeviceFileSystem("sdmc"));
}
#endif

namespace {

#if defined(__SWITCH2__)
// Switch 2 bring-up breadcrumb. A crash before Bootstrap() leaves no normal log,
// so record how far startup got by rewriting sdmc:/switch/toronja/log/boot.txt
// (raw fs, like the exception handler). Absent file => died before main.
//
// The log lives in a plain char buffer on purpose: it is written from
// userAppInit, i.e. BEFORE this translation unit's own static constructors run,
// so it must not be an object with a constructor (a std::string here would be
// reset to empty when its constructor finally ran, dropping the early lines).
char s_boot_log[8192];
std::size_t s_boot_log_len = 0;
u64 s_boot_tick0 = 0;
void Switch2BootMark(const char* stage) {
    const u64 now = armGetSystemTick();
    if (s_boot_tick0 == 0) {
        s_boot_tick0 = now;
    }
    const unsigned long long ms = armTicksToNs(now - s_boot_tick0) / 1000000ULL;
    if (s_boot_log_len < sizeof(s_boot_log) - 1) {
        const int n = std::snprintf(s_boot_log + s_boot_log_len, sizeof(s_boot_log) - s_boot_log_len,
                                    "[%6llums] %s\n", ms, stage);
        if (n > 0) {
            s_boot_log_len += static_cast<std::size_t>(n);
            if (s_boot_log_len > sizeof(s_boot_log) - 1) {
                s_boot_log_len = sizeof(s_boot_log) - 1;
            }
        }
    }
    FsFileSystem* sdmc = fsdevGetDeviceFileSystem("sdmc");
    if (sdmc == nullptr) {
        return;
    }
    fsFsCreateDirectory(sdmc, "/switch");
    MoveOldUserFolder(sdmc);
    fsFsCreateDirectory(sdmc, "/switch/toronja");
    fsFsCreateDirectory(sdmc, "/switch/toronja/log");
    const char* path = "/switch/toronja/log/boot.txt";
    // The first mark of a run keeps the file of the run before it: what a crash left there
    // was otherwise gone the moment the app was opened again.
    static bool s_kept_previous = false;
    if (!s_kept_previous) {
        s_kept_previous = true;
        const char* old_path = "/switch/toronja/log/boot.old.txt";
        fsFsDeleteFile(sdmc, old_path);
        fsFsRenameFile(sdmc, path, old_path);
    }
    fsFsDeleteFile(sdmc, path);
    if (R_FAILED(fsFsCreateFile(sdmc, path, static_cast<s64>(s_boot_log_len), 0))) {
        return;
    }
    FsFile file;
    if (R_FAILED(fsFsOpenFile(sdmc, path, FsOpenMode_Write, &file))) {
        return;
    }
    fsFileWrite(&file, 0, s_boot_log, s_boot_log_len, FsWriteOption_Flush);
    fsFileClose(&file);
}
// If a global constructor aborts (uncaught C++ exception, or abort()/assert
// raising SIGABRT), capture the reason to boot.txt. Installed in userAppInit,
// which runs before __libc_init_array, so it is armed during the ctor phase.
[[noreturn]] void Switch2Terminate() {
    char msg[256];
    const char* what = "(no active exception)";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            std::snprintf(msg, sizeof(msg), "TERMINATE: %s (jit failures=%u last rc=0x%x)",
                          ex.what(), g_oaknut_horizon_jit_failures,
                          g_oaknut_horizon_jit_last_rc);
            Switch2BootMark(msg);
            std::_Exit(42);
        } catch (...) {
            what = "non-std exception";
        }
    }
    std::snprintf(msg, sizeof(msg), "TERMINATE: %s", what);
    Switch2BootMark(msg);
    std::_Exit(42);
}
} // namespace

extern "C" {
// Breadcrumb for other translation units (the game shutdown path in emulation.cpp).
void DekoponBootMark(const char* stage) {
    Switch2BootMark(stage);
}

void Switch2Signal(int sig) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "SIGNAL %d caught during startup", sig);
    Switch2BootMark(buf);
    std::_Exit(43);
}

extern char* fake_heap_start;
extern char* fake_heap_end;

// Runs during libnx/libounce __appInit, before global constructors and main.
void userAppInit(void) {
    Switch2BootMark("00 userAppInit (appInit reached)");
    std::set_terminate(&Switch2Terminate);
    std::signal(SIGABRT, &Switch2Signal);
    std::signal(SIGSEGV, &Switch2Signal);
    std::signal(SIGILL, &Switch2Signal);

    // Environment snapshot: memory budget and launch mode differ a lot between
    // loaders, and a static constructor running out of heap looks like this bug.
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const u32 hos = hosversionGet();
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "env: applet=%d nso=%d heapovr=%lluMB heapleft=%lluMB total=%lluMB used=%lluMB "
                  "hos=%u.%u.%u",
                  static_cast<int>(appletGetAppletType()), envIsNso() ? 1 : 0,
                  static_cast<unsigned long long>(
                      envHasHeapOverride() ? envGetHeapOverrideSize() >> 20 : 0),
                  static_cast<unsigned long long>((fake_heap_end - fake_heap_start) >> 20),
                  static_cast<unsigned long long>(total >> 20),
                  static_cast<unsigned long long>(used >> 20),
                  static_cast<unsigned>(HOSVER_MAJOR(hos)), static_cast<unsigned>(HOSVER_MINOR(hos)),
                  static_cast<unsigned>(HOSVER_MICRO(hos)));
    Switch2BootMark(buf);
}

// --- Global-constructor tracer -------------------------------------------------
// Replaces newlib's __libc_init_array (called at the end of __libnx_init) with a
// copy that records, BEFORE calling each constructor, its index and its offset
// from the module base into sdmc:/switch/toronja/log/ctor.txt (one fixed-size
// record, overwritten in place and flushed). If startup dies inside a global
// constructor - by exception, abort, trap or CPU fault - the record left on the
// SD names the culprit: resolve the offset with docker/switch2/symbolize.py and
// the debug ELF from the same CI run.
typedef void (*Switch2InitFn)(void);
extern Switch2InitFn __preinit_array_start[] __attribute__((weak));
extern Switch2InitFn __preinit_array_end[] __attribute__((weak));
extern Switch2InitFn __init_array_start[] __attribute__((weak));
extern Switch2InitFn __init_array_end[] __attribute__((weak));
void _init(void) __attribute__((weak));
void _start(void);

static FsFile s_ctor_file;
static bool s_ctor_file_open = false;
#define SWITCH2_CTOR_RECORD_SIZE 64

static void Switch2CtorRecord(const char* text) {
    if (!s_ctor_file_open) {
        return;
    }
    char rec[SWITCH2_CTOR_RECORD_SIZE];
    std::memset(rec, ' ', sizeof(rec));
    rec[sizeof(rec) - 1] = '\n';
    const std::size_t len = std::strlen(text);
    std::memcpy(rec, text, len < sizeof(rec) - 1 ? len : sizeof(rec) - 1);
    fsFileWrite(&s_ctor_file, 0, rec, sizeof(rec), FsWriteOption_Flush);
}

// No stack protector here: the last constructor in the list is libounce's
// __stack_chk_init, which sets the canary value. A protected frame that is live
// across that call was set up with the old value and fails its check on return
// (abort -> SIGABRT right after the last constructor). newlib's own
// __libc_init_array has no protected frame for the same reason.
__attribute__((no_stack_protector)) void __libc_init_array(void) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&_start);
    const std::size_t npre = static_cast<std::size_t>(__preinit_array_end - __preinit_array_start);
    const std::size_t ninit = static_cast<std::size_t>(__init_array_end - __init_array_start);
    static char buf[96];

    if (FsFileSystem* sdmc = fsdevGetDeviceFileSystem("sdmc")) {
        const char* path = "/switch/toronja/log/ctor.txt";
        fsFsCreateDirectory(sdmc, "/switch");
        MoveOldUserFolder(sdmc);
        fsFsCreateDirectory(sdmc, "/switch/toronja");
        fsFsCreateDirectory(sdmc, "/switch/toronja/log");
        fsFsDeleteFile(sdmc, path);
        if (R_SUCCEEDED(fsFsCreateFile(sdmc, path, SWITCH2_CTOR_RECORD_SIZE, 0)) &&
            R_SUCCEEDED(fsFsOpenFile(sdmc, path, FsOpenMode_Write, &s_ctor_file))) {
            s_ctor_file_open = true;
        }
    }

    std::snprintf(buf, sizeof(buf), "00a ctor trace: preinit=%zu init=%zu file=%d", npre, ninit,
                  s_ctor_file_open ? 1 : 0);
    Switch2BootMark(buf);

    for (std::size_t i = 0; i < npre; i++) {
        std::snprintf(buf, sizeof(buf), "RUNNING preinit %zu/%zu off=0x%llx", i, npre,
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(__preinit_array_start[i]) - base));
        Switch2CtorRecord(buf);
        __preinit_array_start[i]();
    }
    if (_init) {
        Switch2CtorRecord("RUNNING _init");
        _init();
    }
    for (std::size_t i = 0; i < ninit; i++) {
        std::snprintf(buf, sizeof(buf), "RUNNING init %zu/%zu off=0x%llx", i, ninit,
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(__init_array_start[i]) - base));
        Switch2CtorRecord(buf);
        __init_array_start[i]();
    }

    std::snprintf(buf, sizeof(buf), "ALL DONE init=%zu", ninit);
    Switch2CtorRecord(buf);
    if (s_ctor_file_open) {
        fsFileClose(&s_ctor_file);
        s_ctor_file_open = false;
    }
    Switch2BootMark("00c all global constructors done");
}
} // extern "C"

namespace {
#define SWITCH2_BOOT_MARK(stage) Switch2BootMark(stage)
#else
#define SWITCH2_BOOT_MARK(stage) ((void)0)
#endif

constexpr std::array<std::pair<u64, SwitchFrontend::InputButton>, 16> button_map{{
    {HidNpadButton_A, SwitchFrontend::InputButton::A},
    {HidNpadButton_B, SwitchFrontend::InputButton::B},
    {HidNpadButton_X, SwitchFrontend::InputButton::X},
    {HidNpadButton_Y, SwitchFrontend::InputButton::Y},
    {HidNpadButton_Up, SwitchFrontend::InputButton::Up},
    {HidNpadButton_Down, SwitchFrontend::InputButton::Down},
    {HidNpadButton_Left, SwitchFrontend::InputButton::Left},
    {HidNpadButton_Right, SwitchFrontend::InputButton::Right},
    {HidNpadButton_L, SwitchFrontend::InputButton::L},
    {HidNpadButton_R, SwitchFrontend::InputButton::R},
    {HidNpadButton_Plus, SwitchFrontend::InputButton::Start},
    {HidNpadButton_Minus, SwitchFrontend::InputButton::Select},
    {HidNpadButton_ZL, SwitchFrontend::InputButton::ZL},
    {HidNpadButton_ZR, SwitchFrontend::InputButton::ZR},
    {HidNpadButton_StickL, SwitchFrontend::InputButton::L3},
    {HidNpadButton_StickR, SwitchFrontend::InputButton::R3},
}};

constexpr std::size_t kHandheldSensor = 0;
constexpr std::size_t kFullKeySensor = 1;
constexpr std::size_t kJoyDualLeftSensor = 2;
constexpr std::size_t kJoyDualRightSensor = 3;
constexpr std::size_t kJoyLeftSensor = 4;
constexpr std::size_t kJoyRightSensor = 5;
std::array<HidSixAxisSensorHandle, 6> six_axis_handles{};
bool seven_six_axis_initialized{};
bool seven_six_axis_started{};

void StartSixAxis() {
    hidGetSixAxisSensorHandles(&six_axis_handles[kHandheldSensor], 1, HidNpadIdType_Handheld,
                               HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&six_axis_handles[kFullKeySensor], 1, HidNpadIdType_No1,
                               HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&six_axis_handles[kJoyDualLeftSensor], 2, HidNpadIdType_No1,
                               HidNpadStyleTag_NpadJoyDual);
    hidGetSixAxisSensorHandles(&six_axis_handles[kJoyLeftSensor], 1, HidNpadIdType_No1,
                               HidNpadStyleTag_NpadJoyLeft);
    hidGetSixAxisSensorHandles(&six_axis_handles[kJoyRightSensor], 1, HidNpadIdType_No1,
                               HidNpadStyleTag_NpadJoyRight);
    for (const HidSixAxisSensorHandle& handle : six_axis_handles) {
        hidStartSixAxisSensor(handle);
    }

    seven_six_axis_initialized = R_SUCCEEDED(hidInitializeSevenSixAxisSensor());
    if (seven_six_axis_initialized) {
        seven_six_axis_started = R_SUCCEEDED(hidStartSevenSixAxisSensor());
    }
}

void StopSixAxis() {
    if (seven_six_axis_started) {
        hidStopSevenSixAxisSensor();
        seven_six_axis_started = false;
    }
    if (seven_six_axis_initialized) {
        hidFinalizeSevenSixAxisSensor();
        seven_six_axis_initialized = false;
    }
    for (const HidSixAxisSensorHandle& handle : six_axis_handles) {
        hidStopSixAxisSensor(handle);
    }
}

SwitchFrontend::MotionState ReadSevenSixAxis() {
    if (!seven_six_axis_started) {
        return {};
    }

    HidSevenSixAxisSensorState sensor{};
    std::size_t count{};
    if (R_FAILED(hidGetSevenSixAxisSensorStates(&sensor, 1, &count)) || count == 0) {
        return {};
    }

    // Un-named libnx values that  map to acceleration, angular velocity, and orientation.
    return {
        .active = true,
        .accel_x = sensor.unk_x18[0],
        .accel_y = sensor.unk_x18[1],
        .accel_z = sensor.unk_x18[2],
        .gyro_x = sensor.unk_x18[3],
        .gyro_y = sensor.unk_x18[4],
        .gyro_z = sensor.unk_x18[5],
    };
}

SwitchFrontend::MotionState PollMotion(PadState& pad) {
    const SwitchFrontend::GyroSource source = SwitchFrontend::GetGyroSource();
    if (source == SwitchFrontend::GyroSource::Console) {
        return ReadSevenSixAxis();
    }

    const u64 style_set = padGetStyleSet(&pad);
    // Some compatible Joy-Cons leave the Handheld motion data empty so give them (and old
    // firmwares), a fallback path.
    if ((style_set & HidNpadStyleTag_NpadHandheld) != 0 &&
        source == SwitchFrontend::GyroSource::Automatic) {
        const SwitchFrontend::MotionState motion = ReadSevenSixAxis();
        if (motion.active) {
            return motion;
        }
    }

    HidSixAxisSensorState sensor{};
    bool read = false;

    if ((style_set & HidNpadStyleTag_NpadHandheld) != 0) {
        read = hidGetSixAxisSensorStates(six_axis_handles[kHandheldSensor], &sensor, 1) > 0;
    } else if ((style_set & HidNpadStyleTag_NpadFullKey) != 0) {
        read = hidGetSixAxisSensorStates(six_axis_handles[kFullKeySensor], &sensor, 1) > 0;
    } else if ((style_set & HidNpadStyleTag_NpadJoyDual) != 0) {
        const u64 attributes = padGetAttributes(&pad);
        const bool left_connected = (attributes & HidNpadAttribute_IsLeftConnected) != 0;
        const bool right_connected = (attributes & HidNpadAttribute_IsRightConnected) != 0;
        if (source != SwitchFrontend::GyroSource::RightController && left_connected) {
            read = hidGetSixAxisSensorStates(six_axis_handles[kJoyDualLeftSensor], &sensor, 1) > 0;
        }
        if (!read && source != SwitchFrontend::GyroSource::LeftController && right_connected) {
            read = hidGetSixAxisSensorStates(six_axis_handles[kJoyDualRightSensor], &sensor, 1) > 0;
        }
    } else if ((style_set & HidNpadStyleTag_NpadJoyLeft) != 0 &&
               source != SwitchFrontend::GyroSource::RightController) {
        read = hidGetSixAxisSensorStates(six_axis_handles[kJoyLeftSensor], &sensor, 1) > 0;
    } else if ((style_set & HidNpadStyleTag_NpadJoyRight) != 0 &&
               source != SwitchFrontend::GyroSource::LeftController) {
        read = hidGetSixAxisSensorStates(six_axis_handles[kJoyRightSensor], &sensor, 1) > 0;
    }

    if (!read) {
        return {};
    }
    return {
        .active = true,
        .accel_x = sensor.acceleration.x,
        .accel_y = sensor.acceleration.y,
        .accel_z = sensor.acceleration.z,
        .gyro_x = sensor.angular_velocity.x,
        .gyro_y = sensor.angular_velocity.y,
        .gyro_z = sensor.angular_velocity.z,
    };
}

// Reads the pad into `state` and returns the raw buttons held mask.
// Forwarding the state to the guest is left to the caller so it can withhold input while the quick menu is up.
u64 PollInput(PadState& pad, SwitchFrontend::InputState& state) {
    padUpdate(&pad);
    const u64 held = padGetButtons(&pad);
    const HidAnalogStickState left = padGetStickPos(&pad, 0);
    const HidAnalogStickState right = padGetStickPos(&pad, 1);

    state = SwitchFrontend::InputState{
        .left_x = left.x,
        .left_y = left.y,
        .right_x = right.x,
        .right_y = right.y,
        .motion = PollMotion(pad),
    };
    for (const auto& [source, target] : button_map) {
        if ((held & source) != 0) {
            state.buttons |= SwitchFrontend::ButtonMask(target);
        }
    }

    HidTouchScreenState touch{};
    if (hidGetTouchScreenStates(&touch, 1) != 0 && touch.count > 0) {
        state.touch_pressed = true;
        state.touch_x = touch.touches[0].x;
        state.touch_y = touch.touches[0].y;
    }
    return held;
}

void RunGame(PadState& pad, const std::string& rom) {
    if (!SwitchFrontend::CreateWindow(nwindowGetDefault())) {
        std::printf("EmuWindow no worky.\n");
        SwitchFrontend::SetMenuNotice("Couldn't create the render window");
        return;
    }

    // Each game always starts with the touch pointer off and the quick menu closed.
    SwitchFrontend::ResetPointer();

    if (SwitchFrontend::BootRom(rom)) {
        u64 prev_held = 0;
        u64 prev_input = 0;
        while (appletMainLoop()) {
            // Blocks
            SwitchFrontend::PumpKeyboard();

            SwitchFrontend::UpdateDisplayMode();

            SwitchFrontend::InputState state;
            const u64 held = PollInput(pad, state);
            u64 pressed = held & ~prev_held;
            // The receiver's pad works this console's in-game menu too: its buttons arrive
            // as InputButton bits, its stick as directions.
            static u64 prev_remote = 0;
            u64 remote = 0;
            SwitchFrontend::GameShare::RemotePad remote_pad;
            if (SwitchFrontend::GameShare::GetRemotePad(remote_pad)) {
                static constexpr u64 map[] = {
                    HidNpadButton_A,    HidNpadButton_B,     HidNpadButton_X,  HidNpadButton_Y,
                    HidNpadButton_Up,   HidNpadButton_Down,  HidNpadButton_Left,
                    HidNpadButton_Right, HidNpadButton_L,    HidNpadButton_R,  0,
                    0,                  HidNpadButton_ZL,    HidNpadButton_ZR, 0,
                    0};
                for (int bit = 0; bit < 16; ++bit) {
                    if (remote_pad.buttons & (1u << bit)) {
                        remote |= map[bit];
                    }
                }
                constexpr int far = 16000;
                remote |= remote_pad.stick_y > far ? HidNpadButton_StickLUp : 0;
                remote |= remote_pad.stick_y < -far ? HidNpadButton_StickLDown : 0;
                remote |= remote_pad.stick_x < -far ? HidNpadButton_StickLLeft : 0;
                remote |= remote_pad.stick_x > far ? HidNpadButton_StickLRight : 0;
            }
            const u64 remote_pressed = remote & ~prev_remote;
            prev_remote = remote;
            if (SwitchFrontend::IsQuickMenuOpen()) {
                pressed |= remote_pressed;
            }
            Common::StallWatch::Poll();
            // Its + and - together ask for the menu, as they do here.
            if (SwitchFrontend::GameShare::TakeMenuRequest()) {
                SwitchFrontend::ToggleQuickMenu();
            }
            // Emulator actions are edge-detected in the remappable InputButton space.
            const u64 input_pressed = state.buttons & ~prev_input;

            // The +/- chord toggles the in-game quick menu.
            constexpr u64 chord = HidNpadButton_Plus | HidNpadButton_Minus;
            const bool chord_edge = (held & chord) == chord && (prev_held & chord) != chord;
            if (chord_edge) {
                SwitchFrontend::ToggleQuickMenu();
            }

            const bool menu_open = SwitchFrontend::IsQuickMenuOpen();

            // GameShare prototype: a connected receiver plays too.
            SwitchFrontend::GameShare::MergeRemoteInput(state);

            // While the menu is up the guest sees neutral input.
            SwitchFrontend::UpdateInput(menu_open || chord_edge ? SwitchFrontend::InputState{}
                                                                : state);

            if (menu_open && !chord_edge) {
                // Handheld: the menu answers to a finger as well as to the buttons.
                static bool menu_touching = false;
                HidTouchScreenState menu_touch{};
                const bool menu_finger =
                    hidGetTouchScreenStates(&menu_touch, 1) != 0 && menu_touch.count > 0;
                if (menu_finger && !menu_touching &&
                    SwitchFrontend::QuickMenuTouch(menu_touch.touches[0].x / 1280.0f,
                                                   menu_touch.touches[0].y / 720.0f) ==
                        SwitchFrontend::QuickMenuAction::ExitGame) {
                    break;
                }
                menu_touching = menu_finger;

                const SwitchFrontend::MenuDirections dpad = SwitchFrontend::RotateMenuDirections({
                    .up = (pressed & HidNpadButton_Up) != 0,
                    .down = (pressed & HidNpadButton_Down) != 0,
                    .left = (pressed & HidNpadButton_Left) != 0,
                    .right = (pressed & HidNpadButton_Right) != 0,
                });
                const SwitchFrontend::MenuDirections stick = SwitchFrontend::RotateMenuDirections({
                    .up = (pressed & HidNpadButton_StickLUp) != 0,
                    .down = (pressed & HidNpadButton_StickLDown) != 0,
                    .left = (pressed & HidNpadButton_StickLLeft) != 0,
                    .right = (pressed & HidNpadButton_StickLRight) != 0,
                });
                const SwitchFrontend::QuickMenuNav nav{
                    .up = dpad.up || stick.up,
                    .down = dpad.down || stick.down,
                    .left = dpad.left,
                    .right = dpad.right,
                    .confirm = (pressed & HidNpadButton_A) != 0,
                    .cancel = (pressed & HidNpadButton_B) != 0,
                    .alt = (pressed & HidNpadButton_X) != 0,
                    .alt2 = (pressed & HidNpadButton_Y) != 0,
                    .tab_prev = (pressed & HidNpadButton_L) != 0,
                    .tab_next = (pressed & HidNpadButton_R) != 0,
                    .page_prev = (pressed & HidNpadButton_ZL) != 0,
                    .page_next = (pressed & HidNpadButton_ZR) != 0,
                };
                if (SwitchFrontend::UpdateQuickMenu(nav) ==
                    SwitchFrontend::QuickMenuAction::ExitGame) {
                    break;
                }
            } else if (!menu_open) {
                using SwitchFrontend::ButtonMask;
                using SwitchFrontend::GetMapping;
                using SwitchFrontend::MappableControl;
                // The button bound to Cycle Screen Layout (R3 by default) steps the layout.
                if ((input_pressed & ButtonMask(GetMapping(MappableControl::CycleLayout))) != 0) {
                    SwitchFrontend::CycleScreenLayout();
                }
                // The button bound to Toggle Touch Pointer (L3 by default) toggles pointer mode.
                if ((input_pressed & ButtonMask(GetMapping(MappableControl::TogglePointer))) != 0) {
                    SwitchFrontend::TogglePointerMode();
                }
                // The button bound to Swap Screens (unbound by default) flips the two screens.
                if ((input_pressed & ButtonMask(GetMapping(MappableControl::SwapScreens))) != 0) {
                    SwitchFrontend::ToggleSwapScreens();
                }
            }

            SwitchFrontend::UpdateRealAmiibo();

            prev_held = held;
            prev_input = state.buttons;
            if (!SwitchFrontend::IsRunning()) {
                break;
            }
            // The async GPU thread shares core 0 with this loop.
            svcSleepThread(4'000'000);
        }
        SwitchFrontend::EndRealAmiibo();
        SwitchFrontend::StopRom();
        if (SwitchFrontend::ArticDisconnected()) {
            SwitchFrontend::SetMenuNotice(
                "Artic connection failed. Please check the address and tool/server version");
        } else if (SwitchFrontend::LoadFailed()) {
            SwitchFrontend::SetMenuNotice("Couldn't launch. Please check keys/ROM");
        } else if (rom.starts_with("articinio://") || rom.starts_with("articinin://")) {
            const auto installed = SwitchFrontend::GetSystemFileSetupState();
            const bool complete =
                rom.starts_with("articinio://") ? installed.old3ds : installed.new3ds;
            SwitchFrontend::SetMenuNotice(
                complete ? "System-file setup completed"
                         : "System-file setup stopped before all files were installed",
                !complete);
        }
    } else {
        SwitchFrontend::SetMenuNotice("Couldn't launch. ROM not loadable");
    }

    // Make sure a lingering overlay never survives into the next game or the library menu.
    SwitchFrontend::CloseQuickMenu();
    SwitchFrontend::DestroyWindow();
}

} // namespace

int main(int argc, char* argv[]) {
    MoveOldUserFolder(fsdevGetDeviceFileSystem("sdmc")); // Already done unless start-up skipped it.
    SWITCH2_BOOT_MARK("01 main entered");
    SwitchFrontend::SetUpdaterExecutablePath(argc > 0 && argv[0] != nullptr ? argv[0] : "");
    const bool have_socket = R_SUCCEEDED(socketInitializeDefault());
    if (have_socket) {
        nxlinkStdio();
    }
    // Mount the embedded romfs for deko3D shaders
    const bool have_romfs = R_SUCCEEDED(romfsInit());
    if (!have_romfs) {
        std::printf("Warning: romfsInit() failed.\n");
    }
    SWITCH2_BOOT_MARK("02 socket+romfs");
    if (!Common::Horizon::PinCurrentThread(Common::Horizon::CoreFrontend)) {
        std::printf("Warning: failed to pin frontend thread to core 0.\n");
    }

    if (!SwitchFrontend::InitUsbStorage()) {
        std::printf("Warning: %s\n", SwitchFrontend::UsbStorageError().c_str());
    }

    // Resolve SD-card dirs and create folders/files if not present
    const int launch_count = SwitchFrontend::Bootstrap();
    SWITCH2_BOOT_MARK("03 bootstrap done");
    std::printf("FS & logging up (launch #%d). Logs are located at sdmc:/switch/toronja/log/\n",
                launch_count);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    StartSixAxis();

    SwitchFrontend::InitializeInput();
    SWITCH2_BOOT_MARK("04 input ready - entering menu loop");

    std::string pending_rom = (argc > 1 && argv[1] != nullptr) ? argv[1] : std::string{};

    while (appletMainLoop()) {
        std::string rom;
        if (!pending_rom.empty()) {
            rom = std::move(pending_rom);
            pending_rom.clear();
        } else {
            const SwitchFrontend::MenuResult choice = SwitchFrontend::RunMenu(pad);
            if (choice.action == SwitchFrontend::MenuAction::Exit) {
                SwitchFrontend::GameShare::StopLobby(); // Before the sockets go.
                break;
            }
            rom = choice.path;
        }

        if (!rom.empty()) {
            RunGame(pad, rom);
        }
    }

    SwitchFrontend::ShutdownMenu();
    SwitchFrontend::ShutdownInput();
    StopSixAxis();
    SwitchFrontend::Shutdown();
    Common::StopAllThreadWorkers();
    SwitchFrontend::ShutdownUsbStorage();
    if (have_romfs) {
        romfsExit();
    }
    if (have_socket) {
        socketExit();
    }
    return 0;
}
