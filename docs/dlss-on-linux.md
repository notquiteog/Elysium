<!--
SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# NVIDIA DLSS and DLSS 5 on Linux

Elysium reaches DLSS through [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline), and
Streamline has no Linux build: every asset of its releases is for Windows (`x64`, `aarch64` and
`arm64ec`), and the SDK builds through `build.bat`. `ENABLE_NVIDIA_DLSS` is therefore limited to
`WIN32 AND ARCHITECTURE_x86_64`, the Streamline sources are Win32 throughout
(`LoadLibraryW`, `WinVerifyTrust`, `GetFileVersionInfoW`), and `streamline_dlss_nr.h` says as much:
`DLSS-NR is only available on 64-bit Windows`. The native Linux build has no DLSS at all, and the
Graphics settings hide every DLSS setting there rather than offering knobs that do nothing.

What *is* on Linux is the other half: the driver's own NGX. `libnvidia-ngx.so` carries
`DLSSNR.Available`, `DLSSNR.FeatureInitResult` and `DLSSNR.MinDriverVersion*`, and the Wine bridge
the driver ships (`/usr/lib/x86_64-linux-gnu/nvidia/wine/_nvngx.dll`) forwards them, exporting
`DLSSNR.Available`, `DLSSNR.Width` and `DLSSNR.Height`. So the *Windows* build of Elysium, run
under Proton or Wine, can use DLSS 5 on a Linux host — the same way DLSS reaches Linux games.

## What was measured

On an RTX 5060 Ti (GB206, `arch=0x130`) with driver 615.71.09, under Proton Experimental's
wine-11.0, Streamline 2.13 started and every feature reported itself supported on the adapter:

| Feature | `slIsFeatureSupported` | Plugin |
| --- | --- | --- |
| DLSS (super resolution) | yes | `sl.dlss` 2.13.0, NGX 310.8.0 |
| DLSS 5 neural rendering | yes | `sl.dlss_nr` 2.13.0, NGX 310.8.0 |
| DLSS frame generation | yes | `sl.dlss_g` 2.13.0, NGX 310.8.0 |
| Reflex | yes | `sl.reflex` 2.13.0 |
| PCL | yes | `sl.pcl` 2.13.0 |

`slDLSSNRSetOptions`, the DLSS 5 entry point Elysium calls on every frame, resolves. A Vulkan
device created with all 3 instance and 12 device extensions the features ask for was accepted.
Two further notes: NVIDIA's Authenticode signatures verify correctly under Wine, so the signature
check in `streamline.cpp` is no obstacle; and the 2.14.16 headers Elysium compiles against are
accepted by a 2.13 interposer (`host SDK v2.14.16` against `Streamline v2.13.0`).

This covers start-up and feature support, not frame evaluation: nothing here drove
`slEvaluateFeature` over real images.

## The four things that catch you out

1. **DXVK hides the NVIDIA GPU by default.** With `dxgi.hideNvidiaGpu` left alone, DXVK reports an
   AMD device (`vendor ID 0x1002`), Streamline logs `OTA only enabled with NVIDIA GPUs in the
   system`, and `sl.dlss` refuses with `not supported on current hardware`. Set
   `DXVK_CONFIG="dxgi.hideNvidiaGpu = False; dxgi.nvapiHack = False"`.
2. **Streamline needs DXGI and NVAPI even for a Vulkan app**, to enumerate the adapter. Without
   `dxgi.dll` and `d3d11.dll` (DXVK) and `nvapi64.dll` (DXVK-NVAPI) in the prefix, `slInit` dies
   inside a plugin and returns `eErrorExceptionHandler` (24).
3. **`nvngx.dll` and `_nvngx.dll` must be in the prefix's `system32`**, copied from
   `/usr/lib/x86_64-linux-gnu/nvidia/wine/`. They are the bridge to the Linux driver's NGX.
4. **`NvLowLatencyVk.dll` is not in NVIDIA's Streamline zip.** Without it Reflex logs
   `Failed to load NvLowLatencyVk.dll`, which DLSS frame generation depends on. It comes with the
   NVIDIA Windows driver, so copy it into the `streamline` folder beside the other DLLs.

Steam's own Proton sets up 1–3 for you; they only need doing by hand when driving `wine` directly.
