<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center">
  <img src="dist/branding/elysium_icon_with_logo.png" alt="Elysium" width="600">
</p>

# Elysium

Elysium is an experimental, open-source Nintendo Switch and Switch 2 emulator written in C++. It is a fork of [Lichii](https://github.com/zortos293/Lichii), which builds on [Eglantine](https://github.com/Djihads80/Eglantine), Eden, yuzu and Sudachi. Elysium collects fixes from those projects and from other forks, such as [Orchid](https://github.com/MythosOfs/Orchid) and [PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork), and adds its own.

This is an independent fork, not an official release of any of those projects. Elysium is not affiliated with Nintendo.

## Downloads and setup

On Windows, extract the entire build archive and run **elysium.exe**. Only use games and system files you have legally obtained. Games, keys and firmware are not included.

### Existing saves and settings

Elysium keeps the Eden data directory (`%APPDATA%\eden` on Windows) and its configuration keys, so it shares saves, settings and shader caches with Lichii, Eglantine and Eden. Back up your saves before testing experimental builds.

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

The logo, icon and library artwork in [dist/branding](dist/branding) were created by YOB (yobslab) for Lichii. Windows, Linux and macOS icon resources are generated from that artwork by [Generate-ElysiumIcons.ps1](tools/Generate-ElysiumIcons.ps1).

## Credits and license

Elysium inherits the work of the Lichii, Eglantine, Eden, yuzu and Sudachi projects and their contributors, including Lichii's developer zortos293 and Eglantine's original developer Camille LaVey. Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for applicable license terms.
