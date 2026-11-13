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

## Getting started with VR

Requires an OpenXR runtime with OpenGL support (tested: WiVRn on Linux with a Meta Quest 3) and a headset. Controls are keyboard and mouse, no hand controllers. Without a runtime or headset Patalania runs as a normal desktop viewer.

### Set up WiVRn

1. Install WiVRn on the PC (Flatpak `io.github.wivrn.wivrn`) and start the WiVRn server.
2. Install the WiVRn app on the headset. Follow the official instructions: <https://github.com/WiVRn/WiVRn>
3. Open the WiVRn app on the headset and connect to your PC.
4. WiVRn must run on your real GPU: check the WiVRn server log for the GPU name (it should not say "llvmpipe").

### Using Patalania in VR

1. Start Patalania, log in, and press **Ctrl+Shift+V** to enter VR.
2. The whole viewer interface (menus, chat, inventory, HUDs) is shown on a virtual screen in front of you and is used with the mouse.
3. Put the controllers down so that hand tracking is used. Passthrough windows show your real hands and keyboard.
4. Put both hands on the keyboard for about 1 second to place the keyboard window. It can be shown, moved or measured again in Preferences → VR.

### Keys

| Key | Action |
|---|---|
| Ctrl+Shift+V | VR mode on/off (also while a text field has focus) |
| Hold Esc (about 1.5 s) | Leave VR mode |
| Ctrl+Shift+Space | Hide/show the whole interface |
| Ctrl+Shift+Space, hold | Bring the UI panel in front of you (also sets "straight ahead" for the Alt focus) |
| Ctrl+Shift+Alt+Home / End | Panel bigger / smaller |
| Ctrl+Shift + arrows, Page up/down, Home/End, Insert/Delete, Backspace | Keyboard window: move, height, turn, width, measure again (Alt+Page up/down = depth) |
| M | Mouselook |
| Alt+click | Focus camera |

### Tips

- All VR options are under Preferences → VR.
- The desktop FPS limit does not apply in VR. Use "Half frame rate" in Preferences → VR if your PC struggles.

## Voice

Voice chat uses the SLVoice component included in the package. If voice does not start, check the log (`~/.patalania_x64/logs/Patalania.log`) and open an issue.

## Useful links

- [Privacy](../PRIVACY.md) · [Support](../SUPPORT.md) · [Known limitations](KNOWN_LIMITATIONS.md) · [Credits](../CREDITS.md)
- Building from source: [doc/building_linux.md](../doc/building_linux.md), [doc/building_patalania.md](../doc/building_patalania.md)
