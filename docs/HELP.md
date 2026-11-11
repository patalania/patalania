# Patalania Help

Patalania is based on the [Firestorm viewer](https://www.firestormviewer.org/), so nearly all features (inventory, building tools, radar, AO, preferences, …) work as in Firestorm. Firestorm's documentation describes them in detail: <https://wiki.firestormviewer.org/>.

**Please note:** the Firestorm wiki is written for Firestorm. Menus, names and some options differ in Patalania (see [Known limitations](KNOWN_LIMITATIONS.md)). Patalania comes with **no support**; bugs can be reported on GitHub (see [SUPPORT.md](../SUPPORT.md)) – please not to the Firestorm team or Linden Lab.

## Getting started

1. Download the latest Linux package from the [Releases](https://github.com/patalania/patalania/releases) page and unpack it.
2. Run `./install.sh` (optional, adds a menu entry and icon) or start `./patalania` directly.
3. On first start a notice explains that Patalania is not provided by Linden Lab; read it and confirm.
4. Choose the grid (Second Life Main/Beta, OSGrid, or add your own via **Grid Manager**), enter your account and log in.

Settings and logs live in `~/.patalania_x64/`.

## Other grids (OpenSim)

Open the grid selector at the login screen, choose **Grid Manager**, and add the grid's login URI (ask the grid operator). The list is stored locally; nothing is downloaded.

## VR mode (in development)

Requires an OpenXR runtime with OpenGL support (tested: WiVRn on Linux) and a headset. Controls are keyboard and mouse only. VR settings will be documented here with the first VR release.

## Voice

Voice chat uses the SLVoice component included in the package. If voice does not start, check the log (`~/.patalania_x64/logs/Patalania.log`) and open an issue.

## Useful links

- [Privacy](../PRIVACY.md) · [Support](../SUPPORT.md) · [Known limitations](KNOWN_LIMITATIONS.md) · [Credits](../CREDITS.md)
- Building from source: [doc/building_linux.md](../doc/building_linux.md), [doc/building_patalania.md](../doc/building_patalania.md)
