# dekopon → Switch 2 port: handoff / continue-here notes

Everything needed to continue the Switch 2 port on another machine. All code is
pushed to GitHub (see below); clone the fork and read this file.

Last updated: 2026-11-17 (0.9.4 wip: renamed Toronja; launcher, in-game menu and receiver
reworked without console access - see the first section).

## 0.9.4: Toronja, the new launcher, Toronja Link - WRITTEN BLIND, NOT YET RUN ON A CONSOLE

Everything in this section compiles (Switch 2, the receiver for both consoles) but was written
with nobody at the consoles. Only the new launcher *views* were looked at, as PC renders.
Treat every item as "check it first".

**Name.** The app is now **Toronja** (hbmenu: "Toronja for SW2"), the receiver **Toronja
Link**. Only visible text changed. Kept on purpose so existing installs carry on: the data
folder `sdmc:/switch/dekopon`, ini keys, the release repository `Olmectron/dekopon`, release
asset `dekopon.nro`, and internal identifiers (`DEKOPON_*`, `DekoponBootMark`, ...). The
donation page is gone; General > Release Notes cards end with an About page (Olmectron,
Dekopon by PalindromicBreadLoaf, Azahar / Citra). Source copyright headers are untouched.
Icon: `tools/branding/make_icon.py`.

**Launcher** (`menu.cpp`, with the canvas and font split out to `ui_canvas.h` and the new
views in `ui_views.h`):
- Library = a WiiFlow-style shelf (`DrawHome`/`DrawShelf`): styles Coverflow, Carousel,
  Stack (L/R; "Grid" keeps the old page with the drawer). X turns the selected case round.
  A play, Y search, + details, ZL/ZR jump five, left stick click rescans, - or B opens the
  options. Round buttons in the corners (options, close) and arrows beside the title are
  touchable.
- Covers: `covers.cpp`. Files in `<user dir>/covers/<ID>.jpg|png` (front) and
  `covers/full/` (back + spine + front), `<ID>` = four-character game code (read from the
  NCCH header, `ReadGameCode`) or title ID. Missing ones are fetched from GameTDB in the
  background when the console is online (General > Library > Download Covers). For a console
  kept offline: `python tools/covers/fetch_covers.py --host <ip> --port 5000` on a PC.
  UNVERIFIED: that GameTDB serves `coverfull` for 3DS at that URL; the front `cover` path is
  the fallback.
- Options: one drawer (Settings / Tools / Advanced) replaces the rail and the settings tabs.
  `Category` was reordered: everyday pages first (General, Display, Audio, Controls, Amiibo,
  GameShare, System), then the Advanced ones (`IsAdvancedCategory`). Entries were moved
  between pages with `Table::Page()` in `settings_registry.cpp`, not by changing where they
  are stored. Opening an Advanced page asks once per run; a reminder stays in its header.
  X (right stick in the per-game editor) shows what the focused option is for.
- Known rough edge: the drawer is 268 px wide, which leaves little room when the menu is
  rotated to portrait.

**Seeing the UI without a console.** `tools/ui-preview` renders the views in `ui_views.h` to
PNG; the `ui-preview` workflow does it on every push that touches them and uploads the
pictures as an artifact. It covers the shelf, the home bar, the drawer, the Advanced banner
and the info sheet - not `menu.cpp`'s own pages.

**In-game menu** (`overlay_menu.cpp`, drawn in `renderer_vulkan.cpp` `PrepareQuickMenu`):
pages listed down the side, L/R from anywhere, Graphics / Stereo / System under an Advanced
heading, a line saying what the selected option is for, launcher palette. Opening it pauses
the game by default on every platform.

**Amiibo.** `amiibo_session.cpp` `FollowGuest()`: Amiibo > When A Game Asks For An Amiibo =
scan a real one (the reader opens when the game starts searching and closes ~3 s after it
stops), choose a file (the in-game menu opens on its Amiibo page), or nothing.

**GameShare additions** (protocol in `game_share.h`): both screens in one 400x480 picture;
COMMAND (the receiver asks the host to show top or bottom only, and for its FPS counter);
AUDIO (DSP output tapped in `DspInterface::OutputFrame`, PCM 32728 Hz); STATUS; AMIIBO (a
figure read by the receiver's reader, loaded on the host; writing back is refused). Touch
from the receiver now goes through `EmuWindow::TouchPressedOnScreen`, so it lands even when
the host shows only the top screen.

**Toronja Link** (`tools/gameshare/receiver-nx`, UI in `source/link_ui.h` on the launcher's
canvas): a tab in a corner (tap, swipe from the corner, or hold + and -; it can be hidden
and still works), a menu with the screen pairings, two-screen layouts, sound, FPS here / on
the host, tab opacity and corner, and Close. Settings in `sdmc:/switch/dekopon-link.ini`.

**Switch 1 build.** `docker/switch1/build.sh` + the `dekopon-switch1` workflow (fetches NXVK,
builds its archives once, cached by NXVK commit). New in this round - check the workflow's
last run before relying on it.

**CI.** The toolchain image is cached in Actions (pkg.devkitpro.org answers 403 to runners
too often to build it every time).

## TL;DR status

- **It boots to the dekopon menu on a real Switch 2** (kaiHEN, HOS 20.0.0,
  launched from the normal homebrew menu), as of commit `8a1d0d4`. `boot.txt`
  reaches `04 input ready - entering menu loop` in ~230 ms and the config /
  sysdata / roms folders are created under `sdmc:/switch/dekopon/`.
- **Next step: boot a game.** Nothing past the menu has been exercised yet:
  Vulkan instance/surface/swapchain (OunceVK), the JIT actually executing
  published code, fastmem, audio, input in-game. Put a ROM in
  `/switch/dekopon/roms/` (and `aes_keys.txt` in `/switch/dekopon/sysdata/` for
  encrypted dumps), launch it, and read `log/azahar_log.txt`, `log/boot.txt`
  and `log/crash.txt`.
- The port builds in CI; OunceVK supplies the Vulkan symbols at link time
  (`vkGetInstanceProcAddr`, `VK_NN_vi_surface`).

### Startup bugs fixed on the way (all confirmed on console)
1. **Forced applet type.** dekopon forced `__nx_applet_type =
   AppletType_Application` (title takeover); under a normal kaiHEN launch that
   aborts in `appInit`. Now `AppletType_Default` on `__SWITCH2__`.
2. **JIT unavailable -> `std::bad_alloc` in a global constructor.** dynarmic's
   `spin_lock_arm64.cpp` has a static `oaknut::CodeBlock`; `jitCreate` failed
   because libounce's `Jit` API needs kaiHEN's provider (`-lkaihen`), which
   nothing linked. Fixed in `5edc01b` (see the JIT section below). Found with
   the constructor tracer: `ctor.txt` = `RUNNING init 15/36 off=0x13c900`.
3. **Self-inflicted:** the constructor tracer's frame was live across
   `__stack_chk_init` (last constructor, sets the canary) -> stack-protector
   abort right after the constructors. Fixed in `8a1d0d4`
   (`no_stack_protector`). Keep in mind for any code running around ctors.

## Shader compilation on Switch 2 (async, v0.9.1) - current focus

Games run (OoT3D, MM3D, Kirby: Planet Robobot tested by the user). Work moved to
shader stutter. Data comes from `log/azahar_log.txt`: every 5 s while active the
renderer logs `Compile stats: ...` (render-thread VS generation, shader compiles,
pipeline builds, pipelines served inline from the driver cache, skipped draws,
software-vertex draws) and `JIT publishes (kaiHEN IPC): ...`.

- Device: NVK 26.2.2 (OunceVK), Vulkan 1.4, extended dynamic state + dynamic
  blend + pipeline creation cache control all available; core mask 0x3f.
- **Async Shader Compilation (Experimental) is on by default** on Switch 2.
  Compiles run on the spare cores (3-5), 3 workers per queue. Reset All Settings
  offers "Default" and "Default, no experimental async". Performance / Ultra
  Performance presets are hidden on Switch 2 (they glitch).
- Async mode, pipeline not ready: the draw is skipped, except (a) vertex shader
  still compiling -> software vertex path, (b) driver already has the pipeline
  cached -> bound at once on the render thread (`TryBuild` -> `Build(true)`).
  (b) fixed constant flicker in Kirby, which keeps creating new VS *configs*
  for shaders that already exist (pipelines are keyed by config hash).
- Cache regeneration (shader generator sources or settings changed; the
  version is an MD5 of the files listed in `CMakeModules/GenerateSCMRev.cmake`,
  which includes `vk_shader_disk_cache.*` and `vk_pipeline_cache.*`) used to be
  serial: 89 s for Kirby's 9000-VS cache. Now compiled on the shader workers.
  Loading always shows a "Loading shaders" toast with progress.
- Known / next:
  - Majora's Mask drops to ~18 fps on zone changes with `other` (CPU) at 26 ms
    and GPU at 1 ms - not shaders. Suspect: one kaiHEN IPC per JIT block
    publish (`kaihenPublishJitAlias`); check the `JIT publishes` log line, then
    batch with `kaihenJitPublishRanges` or coalesce per `Emit`.
  - Real glitch-free async needs a fallback for fragment shaders / pipelines
    that are genuinely new: an ubershader. `renderer_deko3d/shaders/
    ubershader_fsh.glsl` + `FSConfigUniformData` (dk_shader.h) are a starting
    point; pair it with the software vertex path so only a handful of
    pipelines need precompiling.
  - Kirby creates thousands of VS configs / a 94 MB driver pipeline cache:
    key pipelines by SPIR-V id instead of config hash.
- Releases: run the `dekopon-switch2` workflow manually with `release_tag`
  (must equal `v` + `DEKOPON_VERSION`); the in-app updater reads
  github.com/Olmectron/dekopon releases and needs the asset `dekopon.nro`.
- Experimental work goes on branch `switch2-async-shaders` (CI builds it too).

## GPU read-backs on Switch 2 (the Majora's Mask "sun" slowdown) - FIXED, confirmed on console

**Symptom.** Majora's Mask 3D dropped from 30 to ~18 fps whenever the sun existed in the
scene and the camera faced where it is (visible or not, even below the horizon at night),
and during day/night transitions in Clock Town. Never in rain or on the final day (no sun).
Independent of resolution. Same on Switch 1.

**What the game does.** While the sun exists it reads the depth buffer back every frame (its
occlusion test for the sun / lens flare): one read-back per frame.

**Root cause.** Switch 2's driver (NVK / OunceVK) exposes **no host-visible memory type that is
also CPU-cached**. The download `StreamBuffer` asks for `HostVisible | HostCoherent |
HostCached`, falls back to `DeviceLocal | HostVisible | HostCoherent`, and ends up on an
uncached mapping. Reading from it is extremely slow: the `memcpy` of one depth buffer from the
download buffer into guest memory (`EncodeTexture` -> `LinearCopy`) took **~37 ms**.

**Fix (commit `6bec86e`, `vk_stream_buffer.cpp`).** When the download buffer cannot get a cached
memory type, back it with ordinary memory: `aligned_alloc` + import through
`VK_EXT_external_memory_host` (`StreamBuffer::TryCreateHostBacked`), the same technique the
screenshot path already used (`TryRenderScreenshotWithHostMemory`). The GPU then writes
straight into normal RAM and the copy costs nothing measurable. Log line to look for at
start-up: `Created Download buffer with size 16384 KiB (imported host memory)`.
Falls back to the old behaviour if the extension is missing or the import fails.

**How it was found (keep this method).** Three fixes were written from deduction and all three
missed. What worked was timing the pieces: `VideoCore::CacheStats` (`cache_stats.h`) and the
`Guest reads of GPU-cached memory ... flushes that reached the rasterizer: N ms total = wait +
download + encode + rest` log line (every 5 s, from `PipelineCache::ReportAsyncStats`). In 5 s
looking at the sun: 95 flushes, 3497 ms total, wait 0, download 0, **encode 3496 ms**.
Measure before changing anything in this path.

**Also in the tree from that investigation, and still active:**

- *Read-backs served one frame late* (`RasterizerCache::DownloadSurface`, `__SWITCH2__`): when
  the guest reads the same region on 3 consecutive frames, it gets the copy recorded on the
  previous frame and a new copy is recorded, instead of `scheduler.Finish()` (6-10 ms on this
  driver). Only for plain guest reads (`FlushRegion` without a target surface). **Trade-off:
  the guest sees that region one frame late.** Not the fix for the sun (the sun was the slow
  memcpy); it saves the Finish wait. If a game misbehaves with per-frame read-backs, this is
  the first thing to disable (`STREAK_TO_DELAY`), at a cost of ~6-10 ms per such frame.
- *Clean-range fast path* (`GPU::FlushRegion`, sync GPU only): every guest read of a
  rasterizer-cached page calls into the cache even when nothing is dirty; the range the last
  flush left clean is remembered together with the cache's dirty generation and those reads
  return at once. MM3D does ~180,000 such reads per second all the time. No accuracy trade-off.
- The user's configuration has **Async GPU off** (`Renderer_AsyncGPU: false`); anything that
  only acts on the GPU-thread path does not run for them. A queued-flush variant for async
  GPU was written, never ran, and was reverted.

**Dead ends, so nobody repeats them:** small CPU writes / "Skip CPU Write" (counted: zero in
the slow periods); the GPU wait in `Surface::Download` (removed, no change); syncing with the
GPU thread (Async GPU is off); the number of guest reads (same when fast); JIT recompilation
(no publishes in the slow periods).

## GameShare (screen streaming to a second console) - network and local wireless work

Dekopon streams one 3DS screen as JPEG over UDP and takes input back. Host side:
`src/citra_switch/game_share.cpp` (+ `game_share_ldn.cpp`, its own translation unit because
`<switch.h>` clashes with the emulator's `Result` / `Service`). Receiver: "Dekopon Link",
`tools/gameshare/receiver-nx` (Switch 1 and Switch 2 `.nro`, built by the `dekopon-link`
workflow); `tools/gameshare/receiver.py` is a PC receiver for testing. Settings live in the
"GameShare" tab (enable, mode).

Protocol: UDP port 28300, magic `DKGS`; HELLO(1), INPUT(2, right stick in bytes 17-20),
FRAME(3, JPEG in chunks of at most 1200 bytes), WELCOME(4).

**Network mode** (both consoles on the same LAN / Wi-Fi): confirmed on console, latency is
barely noticeable.

**Local wireless (LDN), console to console - confirmed on console (Switch 2 host, Switch 1
receiver).** What it took, all found on hardware:

- kaiHEN grants `ldn:u` to the Switch 2 host; `ldnCreateNetwork` with
  `local_communication_id = -1` resolves to the id of the title kaiHEN runs under
  (`0400486024dec000` here). While the network is up the Switch 2 leaves its normal LAN, so
  FTP to it stops answering - that is expected, not a crash.
- The Switch 1 only sees the network with `ldnSetProtocol(LdnProtocol_Unknown3)` (the mixed
  NX + Switch 2 protocol); with `LdnProtocol_NX` the scan returns nothing of ours.
- The scan must filter by scene id only (`0x4453`). Filtering by local communication id
  never matches, because the two consoles run as different titles.
- **`ldnConnect` returned `0xc2cb` (LDN "invalid object") whenever hbmenu on the Switch 1
  was started on top of a game (title override).** The system then checks the network's
  local communication id against that game's NACP list, and the Switch 2's id is never in
  it. `ldn:s` does *not* skip the check (tried: same `0xc2cb` through both services).
  **Starting hbmenu from the Album (applet mode) makes it work**: there is no NACP to load,
  and libnx's `ldn.h` documents that the id is only checked "if NACP loading is successful".
- Dekopon Link writes every step of the join to `sdmc:/switch/dekopon-link.log`; read that
  instead of asking for photos of a scrolling console.

**USB cable - implemented, not offered.** `src/citra_switch/game_share_usb.cpp` (shared with
Dekopon Link) carries the same datagrams over a bulk in/out pair and offers both roles on
both ends: device through usbComms (vendor class, 057e:3000) and host through `usb:hs`.
Tried on console: with a USB-C cable to the Switch 2's bottom port, or to the dock's USB-A
port, the Switch 1 comes up as the *device* (the top port does nothing), so the Switch 2 has
to be the USB host - and under kaiHEN neither service opens there
(`usbCommsInitialize=0x4bb25`, `usbHsInitialize=0xf601`). To take it up again once a loader
grants them: add the third entry back to `kGameShareModeNames`, let `SetMode` accept
`Mode::Usb`, and build Dekopon Link with `-DDEKOPON_LINK_USB` (its Y button). `Status()`
in that file reports the role, what `usb:hs` enumerates and the last failing step.

Remote play extras: the receiver's input counts as user activity on the host (no screen
dimming), its six-axis sensor is forwarded (the console being moved aims), and frames are
held back when the receiver reports falling behind (input is sent 125 times a second, not
once per loop pass - that flooded the wireless link).

Switch 2 as the receiver over LDN is untested (it will hit the same id check if its loader
exposes a NACP).

## CPU-side slowdown fixed: dynarmic's spin lock (Majora's Mask, Sheikah stone visions list)

Symptom: 9 fps, rock steady, in a menu that draws almost nothing; 53 of 56 ms per frame in
"other" (CPU), GPU 0.7 ms. The game busy-waits there on one address with LDREX/STREX,
about 165 000 pairs per frame.

How it was found (worth repeating for the next CPU-side mystery): build with
`DEKOPON_ZONE_PROFILER=ON` (docker/switch2/build.sh) - the zone profile is logged with the
"Perf:" line every 10 s, and `arm_dynarmic.cpp` adds counters for the JIT's callbacks. The
counters showed the exclusive-access storm; timestamps taken on entry to and exit from the
two callbacks then showed 32 + 35 ns inside them but ~250 ns in each stretch between them,
i.e. in dynarmic's own path around the callback.

Cause: `Dynarmic::SpinLock::Lock()` / `Unlock()` (the exclusive monitor's lock, taken twice
per LDREX/STREX pair) each ran `std::call_once`, which is slow on this platform. A ready
flag now skips it; the lock also no longer does SEVL + WFE (20 ns, measured - worth ~10%).
Result on console: 9 fps -> 25-29 fps with the profiler still on.

dynarmic is a submodule we do not own, so the change is
`docker/switch2/patches/dynarmic-spinlock.patch`, applied by build.sh. Dead end: WFE
trapping (it does not).


## JIT on Switch 2 (kaiHEN) — read before touching JIT code

Reference: `libkaihen/JIT.md` in https://github.com/ouncebrew/kaihen.

- The game process may not use the code-mapping syscalls. kaiHEN's RO sysmodule
  does the mapping; `kaihen-jit.o` (linked by `-lkaihen`) provides the weak
  `__nx_jit_*` hooks libounce's `jit.c` looks for. Without it `jitCreate`
  returns `LibnxError_JitUnavailable`.
- **`jitGetRwAddr()` is a staging buffer, NOT an alias of RX.** Bytes written
  there are not executable until published. This differs from Switch 1, where
  dekopon relied on a live RW/RX dual mapping.
- How the port handles it (`externals/switch_stubs/oaknut/horizon_jit.cpp`):
  every JIT writer (dynarmic `AddressSpace`, dynarmic spin lock, PICA shader
  JIT) already ends with `oaknut::CodeBlock::invalidate()`, which calls
  `__builtin___clear_cache()` on the RW range. On `__SWITCH2__` the port
  defines `__clear_cache` itself: a range inside a JIT staging buffer is
  published with `kaihenPublishJitAlias` (+ icache invalidate of RX); any other
  range gets normal cache maintenance. No submodule changes needed.
- Consequences to keep in mind: each publish is an IPC round trip (possible
  performance cost; `kaihenJitPublishRanges` can batch); staging doubles JIT
  memory; code that writes to RW **without** calling `invalidate()` will not
  take effect; do not modify a block another thread is executing.
- Not yet looked at on Switch 2: the dynarmic fastmem arena
  (`src/core/arm/dynarmic/horizon_arena.cpp`) uses `svcMapProcessCodeMemory`
  directly, which the game policy does not allow. It checks syscall hints, so
  it should just report unavailable (fastmem off) - unverified.

## Geometry shaders closed the emulator on Switch 2 (0.9.32) - FIXED, confirmed on console

**Symptom.** Pokémon Omega Ruby closed Toronja (and hbmenu with it, "an error
occurred") at the start of its title screen, right after the language choice.
Every time, also as the first game opened, also at 1x resolution, with OunceVK
20261118 as with the release before it. No `crash.txt`, nothing in `boot.txt`:
the exception and signal handlers do not fire on Switch 2. On Switch 1 the same
build ran the title screen normally.

**Cause.** The ARM64 PICA shader JIT (`shader_jit_a64_compiler.cpp`) assembles
each shader into a `std::vector` and then copies it to executable memory. Of
all the PICA instructions, only the geometry shader ones call into the
emulator from compiled code: `EMIT` calls a host helper, and `EMIT`/`SETEMIT`
load a string for an error path. Those addresses were loaded with oaknut's
`MOVP2R`, which chooses the encoding by how far the target is from the place
the code is *being assembled at*:

- within 1 MB: `ADR` (PC-relative)
- within 4 GB: `ADRP` + `ADD` (PC-relative)
- otherwise: `MOV` of the full 64-bit address (position-independent)

With a vector generator that place is `nullptr + offset`, a number close to
zero, so the test really asks "is the target below 4 GB?". PC-relative code is
only right where it was assembled for, and this code runs somewhere else, so
whenever the answer was yes the call went to a wrong address.

- Switch 1: the program is loaded above 4 GB, `MOVP2R` always took the `MOV`
  form, and the bug never showed.
- Switch 2 (kaiHEN): the PC-relative form was taken. That the program sits
  below 4 GB there is deduced from the code and from the fix working; the load
  address itself was not read.

Vertex shaders never reach this: they have no `EMIT`, and most draws do not run
the PICA shader on the CPU at all (the vertex shader is translated to GLSL and
runs on the GPU). A geometry shader draw is never accelerated, so it always
goes through the JIT. The first such draw still worked because the JIT compiles
on a worker thread and the interpreter runs meanwhile; the second one ran the
compiled code and closed the emulator. That is why the game got one frame
further than expected and the log showed nothing unfinished in the renderer.

**Fix** (commit `e0c5bebf8`). `CallFarFunction` for `VectorCodeGenerator`
(`src/common/aarch64/oaknut_util.h`) and the two string loads in
`Compile_EMIT` / `Compile_SETE` load the address with `MOV` of the immediate.
Same instructions as before on Switch 1, correct ones on Switch 2.

**Rule.** In code assembled into a vector, never use `MOVP2R`, `ADR`, `ADRP`
or `BL` with an absolute host address. Labels inside the same shader are fine:
both ends move together. The `CodeGenerator` overload of `CallFarFunction`
(used with real executable memory) is unaffected.

**What it does not fix.** Anything else. Any game using geometry shaders on
Switch 2 had the same crash and should now run; no other game was retested.

### How it was found: the diagnostic trace (`log/diag.txt`)

With no crash report on Switch 2, the last line of the log is the only
evidence, and the normal log is buffered. Since 0.9.30:

- Create an empty `/switch/toronja/log/diag.txt` on the card. The log then
  starts with `Toronja <version> (diagnostic trace on: log/diag.txt)`.
- While it is on, every log line is written and flushed before the call
  returns (`PushEntry` in `common/logging/backend.cpp`), and `LOG_DIAG(class,
  ...)` lines are emitted: every draw step, texture operation, pipeline build,
  queue submit, shader compile and PICA shader JIT compile.
- Vertex shader GLSL is saved as `log/diag_vs_<id>.glsl` before it is compiled.
- `boot.txt` of the previous run is kept as `boot.old.txt`.
- It is slow (about half speed while loading). Delete `diag.txt` and the
  `diag_vs_*.glsl` files when done.

To trace something new, add `LOG_DIAG` at its begin and end: off, it costs one
atomic load.

## Where everything lives

- **Fork (ours):** https://github.com/Olmectron/dekopon
- **Branch:** `switch2-ounce-devkit`
- **PR (within the fork):** https://github.com/Olmectron/dekopon/pull/1
- **Upstream dekopon:** https://github.com/PalindromicBreadLoaf/dekopon (3DS
  emulator for Switch 1, Azahar-based; Vulkan via NXVK)
- **ounce-devkit (Switch 2 SDK):** https://github.com/ouncebrew/ounce-devkit
  (libounce + OunceVK + `Switch2.cmake` + `switch2vars.sh`)
- **kaiHEN (homebrew loader):** https://github.com/ouncebrew/kaihen

### Working references (known-good ounce-devkit homebrew)
- mGBA: https://github.com/jackspence7/GBA-kaiHEN (EGL/GLES; Docker build recipe,
  hand-written `CMakeToolchain2.txt`, display patch)
- NES: https://github.com/Rodpad/SW2NES (Makefile, `switch2_rules`)
- SNES: https://github.com/Rodpad/SW2SNES (Makefile, `switch2_rules`)

All three: **no forced applet type, no custom exception handler.** Specs can be
either `libnx/switch.specs` or `libounce/switch.specs` — both work.

## What the port changed (commit `0ac8e44`)

- Build with `$DEVKITPRO/libounce/cmake/Switch2.cmake` (sets `NINTENDO_SWITCH2`,
  keeps `CMAKE_SYSTEM_NAME == NintendoSwitch`) instead of the stock Switch 1
  toolchain. Add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- **Graphics NXVK → OunceVK:** removed the `nxvk` submodule; on `NINTENDO_SWITCH2`
  the `nxvk` INTERFACE target links `-lvulkan` (OunceVK) instead of NXVK's
  nouveau archives. The Vulkan bootstrap in
  `src/video_core/renderer_vulkan/vk_platform.cpp` and `vk_lsfg.cpp` uses
  OunceVK's standard `vkGetInstanceProcAddr` on `__SWITCH2__` (not NXVK's
  `vk_icdGetInstanceProcAddr`, no `NVK_*` env vars). Switch 1 path untouched.
- See `PORT_SWITCH2.md` for the full port description and open items.

## Bring-up diagnostics currently in the tree

All guarded by `#if defined(__SWITCH2__)` in
`src/citra_switch/citra_switch.cpp`. **These are temporary; revert once it boots.**
- `AppletType_Default` (was `AppletType_Application`).
- `Switch2BootMark()` — rewrites `sdmc:/switch/dekopon/log/boot.txt` with staged,
  timestamped breadcrumbs. Stages: `00 userAppInit`, `env: ...` (applet type,
  heap override size, memory, HOS version), `00a ctor trace`, `00c all global
  constructors done`, `01 main entered`, `02 socket+romfs`, `03 bootstrap done`,
  `04 input ready - entering menu loop`. The log is a plain char buffer on
  purpose (it is written before this TU's own constructors run).
- `userAppInit()` installs `std::set_terminate` + signal handlers that append the
  abort reason to `boot.txt` (including JIT failure count / last result code).
- **Constructor tracer:** `__libc_init_array` is overridden to write, before each
  global constructor, `RUNNING init i/N off=0x...` to
  `sdmc:/switch/dekopon/log/ctor.txt` (single record, overwritten, flushed).
  If startup dies in a constructor, that record names it: resolve the offset
  against `.init_array` / the symbol table of the debug ELF from the same run.
  It costs one SD write per constructor (36 of them, negligible).
- The custom `__libnx_exception_entry` (`horizon_exception.cpp` +
  `horizon_exception_entry.S`) is ENABLED. It has never fired on Switch 2;
  whether user-mode exception delivery works under kaiHEN is unknown.

## CI build (no local devkitPro needed)

Workflow: `.github/workflows/switch2.yml`. Every push to the branch builds the
`docker/switch2` image (devkitPro + ounce-devkit + ccache) and runs
`docker/switch2/build.sh`, then uploads two artifacts per run:
- `dekopon-switch2-nro` → `dekopon.nro` (flash this).
- `dekopon-switch2-debug` → `citra_switch.elf` (unstripped, ~674 MB, for
  crash/offset symbolication — **match it to the SAME run** as the nro flashed).

CI is trimmed for this branch (commit `91e426f`): the upstream `citra-build`,
`citra-libretro` and `citra-format` workflows skip this branch and its PR; the
Switch 2 workflow runs once per push (no `pull_request` trigger) and **a new
push cancels the build in progress** - so do not push docs-only commits while a
build you need is running. ccache is persisted with `actions/cache`
(precompiled headers are off under ccache).

Build flags (`docker/switch2/build.sh`): `Switch2.cmake` toolchain,
`-DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_LSFG=OFF`.

Get a build:
```bash
# list runs
gh run list --repo Olmectron/dekopon --branch switch2-ounce-devkit --workflow dekopon-switch2
# download the nro + debug ELF from a run id
gh run download <RUN_ID> --repo Olmectron/dekopon -n dekopon-switch2-nro   -D nro
gh run download <RUN_ID> --repo Olmectron/dekopon -n dekopon-switch2-debug -D debug
```

Local build instead of CI: see `docker/switch2/BUILD.md` (needs Docker; on
Windows run from WSL2/Git Bash).

## On-device layout (kaiHEN)

- Copy `dekopon.nro` to `/switch/` on the SD (FTP or card reader). Launch from
  the **normal homebrew menu, NOT title takeover.**
- Logs the port writes: `sdmc:/switch/dekopon/log/boot.txt` (breadcrumbs) and,
  on a CPU fault only, `sdmc:/switch/dekopon/log/crash.txt`.
- To actually run games later (once it boots): ROMs in `/switch/dekopon/roms/`,
  and `aes_keys.txt` in `/switch/dekopon/sysdata/` for encrypted dumps. The
  shared font is embedded in the NRO (no `shared_font.bin` needed).

## Symbolicating a crash offset

If `crash.txt` appears (CPU fault), it has `pc (+0xNNNNNN)` relative to module
base. Resolve it to a function with `docker/switch2/symbolize.py` and the ELF
from the **same run**:
```bash
python -m venv .venv && . .venv/Scripts/activate   # or .venv/bin/activate
pip install pyelftools cxxfilt
python docker/switch2/symbolize.py debug/citra_switch.elf 0xNNNNNN
```
(The script reads `.symtab`; `_start` is at vaddr 0x0, so the offset is the
target vaddr directly. Verified working.)

For the current bug there is **no** `crash.txt` (not a CPU fault) — the
`set_terminate`/signal handlers in diag5 write the reason into `boot.txt`
instead, so no symbolication is needed; just read that line.

## Open items (none exercised yet - next is booting a game)

- **Vulkan runtime surface:** `vk_platform.cpp` uses `VK_NN_vi_surface` +
  `NWindow*` on Switch 2. ounce-devkit's own `examples/vulkan-clear` does
  exactly this (`vkCreateViSurfaceNN` with `nwindowGetDefault()`), so the
  approach is right; still unverified at runtime in dekopon.
- **Custom exception handler** vs libounce: it never fires on Switch 2. Either
  make it libounce-compatible or drop it (dropping disables dynarmic fastmem
  fault recovery → run with fastmem off, slower).
- Clean up all `__SWITCH2__` bring-up diagnostics once it boots.

## Commits on this branch (newest first)

```
8a1d0d4 switch2: no stack protector in the constructor tracer
33a3531 docs: update SWITCH2_HANDOFF.md (ctor tracer result, kaiHEN JIT model, trimmed CI)
5edc01b switch2: make the JIT work under kaiHEN (fixes the pre-main abort)
91e426f ci: build only the Switch 2 NRO on the port branch, with ccache
7d99b16 switch2: trace global constructors to find the pre-main abort (diag6)
8a7bfe7 docs: SWITCH2_HANDOFF.md + crash symbolication script
b91cdb3 switch2: capture ctor-phase abort via set_terminate + signal handlers
63d7423 ci: robustly collect the debug ELF for crash symbolication
007d0f5 switch2: re-enable custom exception handler to capture ctor crash
ccbe73b switch2: disable custom exception entry + earlier breadcrumbs (bring-up)
9e5d842 switch2: use launched applet type + boot breadcrumbs (bring-up)
7c2e891 ci: also upload the unstripped ELF for crash symbolication
8148c10 ci: build the Switch 2 port on GitHub Actions
59f7039 docker/switch2: build environment for the Switch 2 port
0ac8e44 Port to Switch 2 via ounce-devkit (OunceVK instead of NXVK)
```
(`dfba28f` and earlier are upstream dekopon.)
