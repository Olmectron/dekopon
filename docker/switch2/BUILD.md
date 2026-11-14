# Building dekopon for Switch 2 (Docker)

A reproducible Docker environment (devkitPro + ounce-devkit) to compile the
Switch 2 port and iterate on build errors. See `../../PORT_SWITCH2.md` for what
the port does and what still needs verifying.

> This has not been built end-to-end yet. The first run is expected to stop at
> an error (most likely the Vulkan WSI surface or the OunceVK loader symbol).
> That output is exactly what's needed to finish the port — capture it.

## Prerequisites
- **Docker** (Docker Desktop on Windows/macOS, or Docker Engine on Linux).
- On Windows, run the commands below from **WSL2** or **Git Bash** so the shell
  scripts run. A local clone of your fork is bind-mounted into the container.

## 1. Clone your fork
```bash
git clone --recursive https://github.com/Olmectron/dekopon.git
cd dekopon
git checkout switch2-ounce-devkit
```
(`--recursive` skips nxvk automatically — it was removed from `.gitmodules`.)

## 2. Build the image (once, ~slow: it downloads the toolchain)
```bash
docker build -t dekopon-switch2 -f docker/switch2/Dockerfile .
```

## 3. Build the NRO
```bash
docker run --rm -it -v "$PWD:/src" dekopon-switch2 docker/switch2/build.sh
```
On success the NRO lands at `build/switch/src/citra_switch/dekopon.nro`.

## Iterating
The build directory lives in your mounted clone, so builds are incremental:
after editing code (or `git pull`ing a fix), just re-run the step 3 command.

- Force a clean reconfigure: append `--fresh`.
- Open a shell to poke around: `docker run --rm -it -v "$PWD:/src" dekopon-switch2`.

## Notes
- `build.sh` sets `-DENABLE_LSFG=OFF` for first bring-up; drop it once the base
  build works.
- The toolchain is `$DEVKITPRO/libounce/cmake/Switch2.cmake`, which defines
  `NINTENDO_SWITCH2`; that switch selects OunceVK (`-lvulkan`) over NXVK.
- If `install.sh` can't download the devkit at image-build time, pass a specific
  version: `docker build --build-arg OUNCE_DEVKIT_VERSION=20261114 ...`.
