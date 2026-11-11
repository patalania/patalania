# Privacy Statement

*Applies to the Patalania viewer, a free third-party viewer for Second Life and OpenSim grids. Patalania is not provided or supported by Linden Lab.*

## In short

- Patalania has **no servers of its own**, no accounts, no telemetry, no advertising and no crash-report upload.
- Patalania talks only to the grid you log in to and to the services that grid uses (see below).
- Your password and chat logs stay on your computer.

## What Patalania stores on your computer

Everything is kept in `~/.patalania_x64/` (Linux) and is never sent to the Patalania project:

- Settings, window layout, grid list, and – only if you tick "Remember" – your account name and login credentials (kept in a local credential file in that folder).
- Local chat and IM logs (if enabled), snapshots you save, the texture/asset cache and the log files (`logs/Patalania.log`).

You can delete this folder at any time to remove all of it. Logs may contain your avatar name and region names; check them before posting them publicly.

## What is sent over the network

1. **To the grid you choose** (e.g. Second Life by Linden Research, Inc., or an OpenSim grid): login data, your in-world activity, chat, inventory and asset requests. This is how any viewer works; the grid operator's own privacy policy applies. For Second Life see <https://lindenlab.com/privacy>.
2. **Usage statistics to Linden Lab.** Linden Lab's third-party viewer policy requires every viewer to send its standard statistics package to the grid (viewer name and version, frame rate, bandwidth, and basic system information such as operating system, CPU and graphics card). Patalania does not remove it and sends it to nobody else.
3. **Voice chat.** If you use voice, audio is sent to the voice provider the grid uses (for Second Life: Vivox, operated for Linden Lab). Voice is optional.
4. **Web content and media.** Web pages shown inside the viewer (profiles, Marketplace, search, media-on-a-prim, streaming audio) are loaded from the servers they belong to, exactly as in a browser. Those sites see your IP address.
5. **Features you start yourself**, such as uploading a snapshot to Flickr or using a translation service, contact the respective third party only when you use them and after you configured them.

Patalania does **not** contact the Firestorm project's servers and does not download Firestorm's support or client-tag data.

## VR mode

The VR view (in development) runs entirely on your computer. Head tracking data is used only to render the image in your headset and is **never stored or transmitted** to a grid or anyone else.

## Crash reports

Patalania is built without a crash-report service. If the viewer crashes, a report may be written to your local `logs/` folder; nothing is uploaded. You decide whether to attach it to a GitHub issue.

## Children

Second Life and most grids are intended for adults. Patalania does not collect any personal data and has no age-specific features.

## Changes and contact

This statement may change with new versions; the current text is in the repository: <https://github.com/patalania/patalania/blob/main/PRIVACY.md>. Questions: open an [issue](https://github.com/patalania/patalania/issues) (do not post passwords or private data there).
