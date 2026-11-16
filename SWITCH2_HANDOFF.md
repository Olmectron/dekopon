# dekopon → Switch 2 port: handoff / continue-here notes

Everything needed to continue the Switch 2 port on another machine. All code is
pushed to GitHub (see below); clone the fork and read this file.

Last updated: 2026-11-16 (v0.9.1: games run, async shaders).

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
