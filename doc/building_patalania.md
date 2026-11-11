# Patalania – lokaler Linux-Build (ReleaseOS)

Getestet am 2026-11-11 auf Ubuntu 24.04 (i7-14700F, 28 Threads, 31 GB RAM), Basis Firestorm_7.2.5, unverändert.
Ergänzt `doc/building_linux.md` (dort steht der Rest).

## Voraussetzungen
- apt: `cmake libssl-dev gcc-14 g++-14` (zusätzlich zu build-essential, libgl1-mesa-dev, libglu1-mesa-dev, libpulse-dev,
  libxinerama-dev, libxrandr-dev, libfontconfig-dev, libfreetype-dev, python3-venv). Upstream baut 7.2.5 mit GCC 14.
- Python-venv mit Autobuild und `ninja` (pip, kein apt nötig); Autobuild-Variablen aus `FirestormViewer/fs-build-variables`.
- Platz: Build-Ordner ca. 19 GB, dazu Pakete/Quellen; mind. 50 GB frei einplanen.

## Umgebung (z. B. `~/patalania-buildenv/env.sh`)
```bash
python3 -m venv ~/patalania-buildenv/.venv
source ~/patalania-buildenv/.venv/bin/activate
pip install -r requirements.txt ninja
git clone https://github.com/FirestormViewer/fs-build-variables.git ~/patalania-buildenv/fs-build-variables
export AUTOBUILD_VARIABLES_FILE=$HOME/patalania-buildenv/fs-build-variables/variables
export CC=gcc-14 CXX=g++-14
```

## Configure
```bash
autobuild configure -A 64 -c ReleaseOS -- -DVIEWER_CHANNEL:STRING=Patalania-Release -DVIEWER_BINARY_NAME:STRING=patalania-bin \
  -DROOT_PROJECT_NAME:STRING=Patalania -DLL_TESTS:BOOL=FALSE \
  -DUSE_FMODSTUDIO:BOOL=OFF -DUSE_KDU:BOOL=OFF -DOPENSIM:BOOL=ON -DHAVOK_TPV:BOOL=OFF
```
Dauer ca. 1,5 Minuten (lädt die Pakete). Hinweise:
- Die Schalter `--chan`, `--clean`, `--package` aus der Firestorm-Doku funktionieren mit autobuild 3.10 **nicht**
  (werden roh an CMake durchgereicht → „Unknown argument“). Kanal stattdessen mit `-DVIEWER_CHANNEL`.
- Die Konfiguration `ReleaseOS` in `autobuild.xml` setzt weder OPENSIM noch FMOD/KDU/Havok → Flags explizit angeben.
- `VIEWER_BINARY_NAME` und `ROOT_PROJECT_NAME` sind CMake-Cache-Variablen: In einem bereits konfigurierten Build-Ordner
  müssen sie (wie oben) explizit übergeben werden, sonst bleibt der alte Name `firestorm-bin`/`Firestorm` im Cache.
- Ohne `ninja` im PATH scheitert CMake („unable to find Ninja“).

## Build – mit Speichergrenze!
`autobuild build` startet ninja ohne Jobgrenze (nproc+2 = 30 Compiler) und hat bei wenig freiem RAM den Rechner
eingefroren. Deshalb ninja direkt, begrenzt:
```bash
systemd-run --user --scope -p MemoryHigh=14G -p MemoryMax=18G -p MemorySwapMax=0 nice -n 10 \
  ninja -C build-linux-x86_64 -j 8 >> build-compile.log 2>&1
```
(`MemorySwapMax=0` ist nötig, sonst greift die Grenze nicht.) Dauer ca. 20 Minuten mit -j 8 für die ersten ~1355 Schritte
nach Fortsetzung; Ergebnis liegt in `build-linux-x86_64/newview/packaged/` und als `.tar.xz` (ca. 200 MB).

## Starten
```bash
cd build-linux-x86_64/newview/packaged && ./patalania
```
Einstellungen, Cache und Logs liegen in `~/.patalania_x64` (getrennt von einer vorhandenen Firestorm-Installation
unter `~/.firestorm_x64`). Der Build ruft noch Firestorm-Dienste (phoenixviewer.com/fsdata) auf – das schaltet Phase 3 ab.
Sichtbares Branding prüfen: `scripts/pat_branding_check.sh`.
