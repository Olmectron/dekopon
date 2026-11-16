# dekopon → Switch 2 port: handoff / continue-here notes

Everything needed to continue the Switch 2 port on another machine. All code is
pushed to GitHub (see below); clone the fork and read this file.

Last updated: 2026-11-16.

## TL;DR status

- The port **builds, links, and produces a valid NRO** in CI. OunceVK supplies
  the Vulkan symbols at link time (`vkGetInstanceProcAddr`, `VK_NN_vi_surface`).
- On a real Switch 2 (kaiHEN / OunceShell), it **does not boot yet.** We are
  debugging startup, iterating via CI builds the user flashes.
- **Bug #1 — FIXED:** dekopon forced `__nx_applet_type = AppletType_Application`
  (title takeover). Under a normal kaiHEN applet launch this aborts in `appInit`
  **before `main`** (no SD folders created). Changed to `AppletType_Default` on
  `__SWITCH2__`. The working references (mGBA, SW2NES, SW2SNES) force no applet
  type — launch is the normal hbmenu, **not** title takeover.
- **Bug #2 — CURRENT BLOCKER:** after `appInit`, it dies in a **global C++
  constructor**, before `main`. Breadcrumb log `boot.txt` reaches
  `00b early ctor` but never `01 main entered`. It is **not a CPU fault** (the
  custom exception handler never fires; no `crash.txt`), so it is an `abort()` /
  assert / uncaught C++ exception. ~2 s delay = other ctors run, then one aborts.
- **Next step (in flight):** build **diag5** (commit `b91cdb3`) installs
  `std::set_terminate` + `SIGABRT/SIGSEGV/SIGILL` handlers in `userAppInit`
  (runs before the constructors) to write the exception message into `boot.txt`.
  Flash it, launch normally, and read the new line in
  `sdmc:/switch/dekopon/log/boot.txt` — it should name the failing subsystem.

## Where everything lives

- **Fork (ours):** https://github.com/Olmectron/dekopon
- **Branch:** `switch2-ounce-devkit`  (remote HEAD: `b91cdb3`)
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
- `Switch2BootMark()` — rewrites `sdmc:/switch/dekopon/log/boot.txt` with staged
  breadcrumbs. Stages: `00 userAppInit`, `00b early ctor`, `01 main entered`,
  `02 socket+romfs`, `03 bootstrap done`, `04 input ready - entering menu loop`.
- `userAppInit()` installs `std::set_terminate` + signal handlers that append the
  abort reason to `boot.txt` (commit `b91cdb3`).
- The custom `__libnx_exception_entry` (`horizon_exception.cpp` +
  `horizon_exception_entry.S`) is currently ENABLED (it writes `crash.txt` on a
  CPU fault). It was briefly disabled (commit `ccbe73b`) then re-enabled
  (`007d0f5`); it is **not** the cause and does not catch this abort.

## CI build (no local devkitPro needed)

Workflow: `.github/workflows/switch2.yml`. On every push to the branch it builds
the `docker/switch2` image (devkitPro + ounce-devkit) and runs
`docker/switch2/build.sh`, then uploads two artifacts per run:
- `dekopon-switch2-nro` → `dekopon.nro` (flash this).
- `dekopon-switch2-debug` → `citra_switch.elf` (unstripped, ~674 MB, for
  crash/offset symbolication — **match it to the SAME run** as the nro flashed).

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

## Suspects / ideas for bug #2 (ctor abort)

- An uncaught C++ exception or assert in a global constructor (most likely).
  diag5's `boot.txt` message should name it.
- Ruled out as ctor-time: the dynarmic fastmem arena (4 GiB virtual reservation)
  is created on game load (`core.cpp` `EnableFastmem`), not at static init.
- If `set_terminate` doesn't fire (abort via `svcBreak`/`diagAbort`), bracket the
  no-priority ctor batch by adding `Switch2BootMark` calls at the top of a few
  key translation units (e.g. `core.cpp`, `memory.cpp`, settings/logging init)
  to localize which module's ctors run last before the abort.

## Open items beyond bug #2 (not yet reached)

- **Vulkan runtime surface:** `vk_platform.cpp` uses `VK_NN_vi_surface` +
  `NWindow*` on Switch 2 (assumption). Unverified at runtime — only hit after
  `main`, once a ROM boots. May need a different WSI if OunceVK differs.
- **Custom exception handler** vs libounce: it never fires on Switch 2. Either
  make it libounce-compatible or drop it (dropping disables dynarmic fastmem
  fault recovery → run with fastmem off, slower).
- Clean up all `__SWITCH2__` bring-up diagnostics once it boots.

## Commits on this branch (newest first)

```
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
