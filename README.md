<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center">
  <img src="dist/branding/elysium_icon.png" alt="Elysium" width="256">
</p>

# Elysium

Elysium is an experimental, open-source Nintendo Switch and Switch 2 emulator written in C++. It is a fork of [Lichii](https://github.com/zortos293/Lichii), which builds on [Eglantine](https://github.com/Djihads80/Eglantine), Eden, yuzu and Sudachi. Elysium collects fixes from those projects and from other forks, such as [Orchid](https://github.com/MythosOfs/Orchid) and [PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork), and adds its own.

This is an independent fork, not an official release of any of those projects. Elysium is not affiliated with Nintendo.

## Downloads and setup

On Windows, extract the entire build archive and run **elysium.exe** (or `elysium-cli.exe` from a terminal). Only use games and system files you have legally obtained. Games, keys and firmware are not included.

### Existing saves and settings

Elysium uses Eglantine's data directory (`%APPDATA%\eglantine` on Windows) and configuration keys, so it shares saves, settings and shader caches with Eglantine. At first start it offers to migrate data from an older `eden` directory (Eden, Lichii and earlier Elysium builds). Back up your saves before testing experimental builds.

Each game's pipeline cache carries over across Elysium updates. To move it to another PC, right-click the game and choose **Export Pipeline Cache**, then **Import Pipeline Cache** on the other PC.

## NVIDIA DLSS, DLSS 5 and DLSS Frame Generation (Windows, experimental)

Elysium can use NVIDIA DLSS through NVIDIA Streamline on GeForce RTX cards. Elysium does not ship NVIDIA's files: you supply them, and Elysium only loads DLLs that carry NVIDIA's signature.

| Feature | What it does | GPU |
|---|---|---|
| DLSS (super resolution) | Replaces a Switch 2 game's own DLSS network (currently Fire Emblem: Fortune's Weave) | RTX 20 series or newer |
| DLSS 5 neural rendering | Runs NVIDIA's DLSS 5 neural rendering model on every presented frame | RTX 50 series |
| DLSS Frame Generation | Inserts generated frames at presentation (2x on RTX 40, up to 4x on RTX 50) | RTX 40 series or newer |

### 1. Install NVIDIA's files

Copy these DLLs from an NVIDIA Streamline package into `%APPDATA%\eglantine\streamline` (you can also use a `streamline` folder next to `elysium.exe`, or set `EDEN_STREAMLINE_DIR`):

- Always: `sl.interposer.dll`, `sl.common.dll`
- DLSS: `sl.dlss.dll`, `nvngx_dlss.dll`
- DLSS 5: `sl.dlss_nr.dll`, `nvngx_dlssnr.dll` (Streamline 2.13 or newer with DLSS 310.8 or newer)
- Frame Generation: `sl.dlss_g.dll`, `nvngx_dlssg.dll`, `sl.reflex.dll`, `sl.pcl.dll`

All `sl.*.dll` files must come from the same Streamline release. The download button in **Graphics** settings fetches the public Streamline release, but that release does not include DLSS 5.

Frame Generation also needs **Hardware-accelerated GPU scheduling**, which you turn on in Windows Settings under System > Display > Graphics.

### 2. Turn the features on

All three settings are in **Emulation > Configure > Graphics**, and they take effect when a game starts.

- **DLSS 5:** check **NVIDIA DLSS 5 neural rendering**, then set **DLSS 5 Intensity** (100% is the model's default strength; lower values weaken the effect).
- **Frame Generation:** check **Enable Frame Generation**, set **Frame Generation Type** to **NVIDIA DLSS Frame Generation**, and choose a **Frame Generation Multiplier** of 2x, 3x or 4x. If your GPU supports fewer generated frames, Elysium uses the highest it can.
- **DLSS super resolution:** check **Use NVIDIA DLSS in place of a game's DLSS**.

The log (`elysium_log.txt`) shows whether each feature started, for example `NVIDIA Streamline attached: DLSS off, DLSS 5 neural rendering on, DLSS frame generation on`. If something is missing, the log says which file or requirement failed. Set `EDEN_NX2_DLSS_LOG=1` (or `2` for verbose output) to include Streamline's own messages.

### Limitations

- Emulated games don't hand over depth or motion vectors for the final frame, so DLSS 5 and Frame Generation receive a flat depth and zero motion. They rely on NVIDIA's own image analysis, and fast motion or HUD elements can show artifacts.
- DLSS 5 runs on SDR output only; it is skipped while HDR output is active.
- DLSS 5's Streamline interface is not public. Elysium declares it using the layout that [openNR](https://github.com/clshortfuse/openNR) recovered from Streamline 2.13, so other versions may not work.
- These features run only with Vulkan on Windows x64.

## Compatibility and issue reports

Compatibility and performance vary by game, hardware and driver. Rendering errors, stutters and crashes remain possible. Passing automated tests does not establish that a game renders correctly, and this project makes no universal frame-rate guarantee.

When [reporting an issue](https://github.com/notquiteog/Elysium/issues), include:

- The exact build, game version, CPU, GPU and driver version.
- Relevant settings and steps to reproduce the problem.
- A screenshot and the log from the affected session, after checking it for personal information.

Do not upload game files, keys, firmware or private account information.

## Building and contributing

See the [build guide](docs/Build.md), [user documentation](docs/user/README.md) and [coding guidelines](docs/Coding.md). Some inherited documentation and internal build target names still use upstream names. The GUI build target remains `yuzu`; its output is `elysium.exe` on Windows and `elysium` on other desktop platforms.

Keep changes focused and distinguish code-level checks from actual gameplay testing.

## Artwork

The app icon is [dist/branding/elysium_icon.png](dist/branding/elysium_icon.png). Windows, Linux and macOS icon resources are generated from it by [Generate-ElysiumIcons.ps1](tools/Generate-ElysiumIcons.ps1). The other logo and library artwork in [dist/branding](dist/branding) was created by YOB (yobslab) for Lichii.

## Credits and license

Elysium inherits the work of the Lichii, Eglantine, Eden, yuzu and Sudachi projects and their contributors, including Lichii's developer zortos293 and Eglantine's original developer Camille LaVey. Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for applicable license terms.
