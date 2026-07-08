<div align=center>

<img src="extras/banner.png" alt="Banner" width="20%">

</div>
<h1 align=center>Very Little Nightmares — Nintendo Switch port</h1>

A wrapper/port of the Android release of **Very Little Nightmares** (v1.2.6). It
loads the original game binaries (`libunity.so` / `libil2cpp.so`, Unity 2021.3
IL2CPP, arm64), resolves their imports against native Switch implementations and
patches them so the game runs as if inside a minimal Android environment.

## How to install

Create a folder for the game on your SD card, `/switch/vln_nx/`, and place:

1. `vln_nx.nro`
2. `libmain.so`, `libunity.so`, `libil2cpp.so` — extracted from the APK's
   `lib/arm64-v8a/` folder.
3. `assets/` — the **contents** of the APK's `assets/` folder (the il2cpp
   metadata, `boot.config`, the small `data.unity3d` stub, …).
4. `main.147.eu.bandainamcoent.verylittlenightmares.obb` — the game's OBB
   expansion file, dropped in as-is.

```
/switch/vln_nx/
  vln_nx.nro
  libmain.so  libunity.so  libil2cpp.so
  assets/
    bin/Data/ ...
  main.147.eu.bandainamcoent.verylittlenightmares.obb
```

On **first launch** the port unpacks the OBB and merges the game data on the
console then deletes the OBB to reclaim space. 
Every later boot is instant.

Launch with a **game override** (hold R while starting a title) or a
forwarder.

## Configuration

`config.txt` is created next to the `.nro` on first run:

* `screen_width` / `screen_height` — render resolution; `-1` (default) picks
  720×1280 in handheld and 1080×1920 docked.
* `portrait` — the render is rotated 90° to fill the screen (hold the console
  rotated to play): `1` (default) rotates clockwise (**right** Joy-Con up); `2`
  rotates the other way (left Joy-Con up).
* `language` — `0` (default) follows the Switch system language;

## Build

devkitA64 plus these portlibs:

```
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib
```

Then `make`. See `BUILD.md` for the full toolchain and data-staging details.

## Credits

* **TheOfficialFloW** & **Andy Nguyen** — the original Android so-loader.
* **fgsfds** — the Switch so-loader groundwork reused here.
* **ChanseyIsTheBest** - Help on Unity / Zookeeper DX Port

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

## Legal

No affiliation with Bandai Namco or Tarsier Studios. "Very Little Nightmares" is
a trademark of its owner. This repository contains no assets or program code
from the original game, and none may be distributed with builds. Users must
extract the required files from their own legally obtained copy.

Source code is provided under the MIT License (see LICENSE).
