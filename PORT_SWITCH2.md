# Dekopon on Switch 2 (ounce-devkit)

This documents the mechanical port of dekopon from the Switch 1 toolchain to
Nintendo Switch 2 using [ounce-devkit](https://github.com/ouncebrew/ounce-devkit)
(libounce + OunceVK). It is **not** a verified build: the changes below compile
in principle but have not been built against a real ounce-devkit install or run
on hardware. Treat the graphics bring-up as the part that still needs hands-on
verification.

## What changed

### Toolchain
- Build with `$DEVKITPRO/libounce/cmake/Switch2.cmake` (after
  `source $DEVKITPRO/switch2vars.sh`) instead of `$DEVKITPRO/cmake/Switch.cmake`.
- That toolchain keeps `CMAKE_SYSTEM_NAME == NintendoSwitch` and sets
  `NINTENDO_SWITCH2`, so dekopon's existing `if (CMAKE_SYSTEM_NAME STREQUAL
  "NintendoSwitch")` block and every `#ifdef __SWITCH__` still fire. The devkit
  defines both `__SWITCH__` and `__SWITCH2__` at the compiler level.

### Graphics driver: NXVK → OunceVK
NXVK is a Switch 1 (Maxwell / Tegra X1, nouveau) Vulkan driver and does not run
on Switch 2's GPU. OunceVK is the Switch 2 Vulkan provider.

- `.gitmodules`: removed the `nxvk` submodule.
- `externals/CMakeLists.txt`: on `NINTENDO_SWITCH2`, the `nxvk` INTERFACE target
  now links `-lvulkan` (OunceVK) instead of the NXVK prebuilt archives. The
  target name `nxvk` is kept so `src/citra_switch` links the same target on both
  platforms. The Switch 1 NXVK archive path is preserved under `elseif`.
- `src/video_core/renderer_vulkan/vk_platform.cpp` and `vk_lsfg.cpp`: the Vulkan
  dispatcher was initialised from NXVK's statically-linked ICD symbol
  `vk_icdGetInstanceProcAddr` plus the `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER` env
  var. On `__SWITCH2__` it is now initialised from OunceVK's standard exported
  `vkGetInstanceProcAddr`, with no NVK/Mesa env vars. Both files branch
  `#if defined(__SWITCH2__) ... #elif defined(__SWITCH__)`, so Switch 1 is
  untouched.

### Docs / build system
- `README.md`: split build instructions into Switch 2 (ounce-devkit, default)
  and Switch 1 (NXVK).
- `CMakeLists.txt`: comments/cache docstrings updated to say OunceVK on Switch 2.

## What is NOT done / needs verification

1. **Window surface (the main risk).** `CreateSurface` / `GetInstanceExtensions`
   in `vk_platform.cpp` use `VK_NN_vi_surface` with a libnx `NWindow*`. This is
   kept for Switch 2 on the assumption that OunceVK/libounce present through the
   same VI/NWindow system. If OunceVK exposes a different WSI extension, the
   `VK_USE_PLATFORM_VI_NN` block and the `createViSurfaceNN` call must be
   branched on `__SWITCH2__`. Marked with a `TODO(switch2)` in the source.
2. **OunceVK loader symbol.** Assumes `-lvulkan` exports `vkGetInstanceProcAddr`
   with the standard prototype. Confirm against OunceVK's headers; if it exports
   a different bootstrap entry (e.g. an ICD symbol like NXVK), adjust the extern
   declarations in `vk_platform.cpp` and `vk_lsfg.cpp`.
3. **libnx vs libounce API gaps.** dekopon uses libnx services (applets, swkbd,
   NFC/amiibo, VI, nvdrv, low-level `horizon_*` stubs). libounce is
   libnx-compatible but new; any missing or changed Switch 2 service surfaces
   only at link/run time.
4. **JIT libraries.** `dynarmic-nx`, `oaknut-nx`, `cryptopp-modern-nx` target
   aarch64 and should carry over (Switch 2 is also aarch64), but W^X / JIT
   memory permissions and CPU feature assumptions have not been checked on
   Switch 2.
5. **LSFG frame generation.** Rewired symbolically but not validated; consider
   building with `-DENABLE_LSFG=OFF` for first bring-up.
6. **No build/run verification.** None of this was compiled with ounce-devkit or
   run on Switch 2 hardware. Expect iteration on link errors.

## Suggested bring-up order
1. Configure + compile with the ounce toolchain; fix link/compile errors.
2. Get past Vulkan instance creation (loader symbol, item 2).
3. Get a valid surface + swapchain (item 1) — a cleared frame on screen.
4. Boot a title; then revisit LSFG, USB, amiibo.
