# Patalania

**Patalania** is a third-party viewer for 3D virtual worlds such as Second Life and OpenSim grids. Its goal is a native OpenXR VR view on Linux (via WiVRn, e.g. Meta Quest 3) that is controlled with keyboard and mouse.

> **Status: in development.** There are no releases yet. The VR view is not implemented yet; for now Patalania behaves like its base.

## Disclaimer

**This software is not provided or supported by Linden Lab, the makers of Second Life.**

Second Life is a trademark of Linden Research, Inc. Patalania is not affiliated with or sponsored by Linden Research, nor by The Phoenix Firestorm Project, Inc. Patalania is intended for use with the Second Life® world and OpenSim grids.

## Base and license

Patalania is a fork of the [Firestorm viewer](https://github.com/FirestormViewer/phoenix-firestorm) (© The Phoenix Firestorm Project, Inc.), which is derived from the official [Second Life viewer](https://github.com/secondlife/viewer) by Linden Lab. The code is based on the official release branch `Firestorm_7.2.5` and licensed under the LGPL 2.1 (see [LICENSE](LICENSE)). All copyright notices of the original authors are retained. See [CREDITS.md](CREDITS.md).

## Information

- [Help](docs/HELP.md) – getting started, other grids, VR
- [Known limitations](docs/KNOWN_LIMITATIONS.md) – differences from Firestorm (no FMOD/Kakadu/Havok, Linux only, …)
- [Privacy statement](PRIVACY.md) – no telemetry, no crash upload; only the grid you log in to
- [Support](SUPPORT.md) – none; bug reports via GitHub issues are welcome
- [Contributing](CONTRIBUTING.md)

## Build

Linux build notes: [doc/building_linux.md](doc/building_linux.md) (base) and [doc/building_patalania.md](doc/building_patalania.md).
