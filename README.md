# Patalania

**Patalania** is a third-party viewer for 3D virtual worlds such as Second Life and OpenSim grids. Its goal is a native OpenXR VR view (currently tested with WiVRn on Linux, e.g. Meta Quest 3) that is controlled with keyboard and mouse.

> **Status: in development.** There are no releases yet. The VR view is not implemented yet; for now Patalania behaves like its base.

## Disclaimer

**This software is not provided or supported by Linden Lab, the makers of Second Life.**

Second Life is a trademark of Linden Research, Inc. Patalania is an independent project; it is not affiliated with, endorsed by or sponsored by Linden Research, nor by The Phoenix Firestorm Project, Inc. Patalania is intended for use with the Second Life® world and OpenSim grids.

## Base and license

Patalania is a fork of the [Firestorm viewer](https://github.com/FirestormViewer/phoenix-firestorm) (© The Phoenix Firestorm Project, Inc.), which is derived from the official [Second Life viewer](https://github.com/secondlife/viewer) by Linden Lab. The code is based on the official release branch `Firestorm_7.2.5` and licensed under the LGPL 2.1 (see [LICENSE](LICENSE)). All copyright notices of the original authors are retained. See [CREDITS.md](CREDITS.md).

## Information

- [Help](docs/HELP.md) – getting started, other grids, VR
- [Known limitations](docs/KNOWN_LIMITATIONS.md) – differences from Firestorm (no FMOD/Kakadu/Havok, …)
- [Privacy statement](PRIVACY.md) – no telemetry, no crash upload; only the grid you log in to
- [Support](SUPPORT.md) – none; bug reports via GitHub issues are welcome
- [Contributing](CONTRIBUTING.md)

## Install (Linux)

Download the `.tar.xz` from the Releases page, extract it, then run `./install.sh` in the extracted folder.
As a normal user it installs to `~/.local/opt/patalania`, with sudo to `/opt/patalania`, and adds a "Patalania" entry to the start menu.
You can also run `./patalania` directly from the extracted folder.

## Quick start (VR)

1. Extract the archive and run `./install.sh` (adds "Patalania" to the start menu).
2. Install WiVRn on the PC (Flatpak `io.github.wivrn.wivrn`) and start the WiVRn server. Install the WiVRn app on the headset. Follow the official WiVRn instructions: <https://github.com/WiVRn/WiVRn>
3. Open the WiVRn app on the headset and connect to your PC.
4. Start Patalania, log in, and press **Ctrl+Shift+V** to enter VR.
5. Most important keys:
   - **Ctrl+Shift+V**, or hold **Esc** for about 1.5 seconds: leave VR
   - **Ctrl+Shift+Space**: show/hide the UI (hold it to bring the UI in front of you)
   - Put both hands on the keyboard for about 1 second: places the keyboard window
   - **M**: mouselook; **Alt+click**: focus camera
   - More settings: Preferences → VR
6. Tips:
   - Put the controllers down so that hand tracking is used.
   - WiVRn must run on your real GPU: check the WiVRn server log for the GPU name (it should not say "llvmpipe").
   - The desktop FPS limit does not apply in VR. Use "Half frame rate" in Preferences → VR if your PC struggles.

## Build

Linux build notes: [doc/building_linux.md](doc/building_linux.md) (base) and [doc/building_patalania.md](doc/building_patalania.md).
