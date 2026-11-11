# Known Limitations

Patalania differs from the official Second Life viewer and from Firestorm in the following ways.

## Build variant

- **Audio:** built without the proprietary FMOD library; sound and music use OpenAL. Fewer audio features than Firestorm's FMOD builds, and OpenAL on Linux is less well maintained.
- **Texture decoding:** built without the proprietary Kakadu (KDU) library; JPEG 2000 textures are decoded with OpenJPEG, which is slower, so textures may load more slowly and use more CPU.
- **Physics tools:** built without Havok. Pathfinding/navmesh editing is unavailable, and physics shapes for mesh uploads are generated differently and are more limited.
- **Voice:** uses the Vivox "SLVoice" component shipped with the viewer (see [CREDITS.md](../CREDITS.md)). Voice on Linux depends on that component working on your system.

## Platform

- Currently available builds: Linux x86_64.
- **VR mode** (in development): currently requires an OpenXR runtime with OpenGL support (tested: WiVRn on Linux with Meta Quest 3), controlled with keyboard and mouse – no hand controllers. If no OpenXR runtime or headset is found, the viewer runs as a normal desktop viewer.

## Services removed

- No connection to Firestorm servers: no Firestorm support-group integration, client-tag database, grid list download, blocked-version list or Firestorm login page. A small local login page is shown instead.
- Grid list: Second Life Main, Second Life Beta and OSGrid are pre-installed. Other grids can be added in the grid manager by login URI.
- Primfeed sharing is not available (disabled at build time).
- No crash-report upload and no automatic update check; new versions are published at <https://github.com/patalania/patalania/releases>.

## Compatibility notes

- The in-world LSL bridge is named "#Patalania LSL Bridge". A Firestorm bridge and a Patalania bridge replace each other if both are worn.
- Patalania uses its own settings folder `~/.patalania_x64/` and does not read or change Firestorm's.
- Features inherited from Firestorm may reference Firestorm's wiki or support groups in their descriptions; those are not Patalania channels and Patalania has no support.
