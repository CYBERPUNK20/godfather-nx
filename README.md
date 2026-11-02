# The Godfather for Nintendo Switch

This project is a static recompilation port of *The Godfather* (original Xbox) for the Nintendo Switch, based on the static recompilation architecture and runtime framework.

The target executable is the original Xbox release (XBE title ID `4541007D`). 

> **Disclaimer:** This repository does NOT contain the game, its assets, binaries, or copyrighted code. You must provide your own legally obtained Xbox game dump.

---

## 🎮 Current Status

- **Status:** **PLAYABLE** on Nintendo Switch!
- **Recompilation:** All x86 execution partitions generated, verified, and running smoothly.
- **Graphics & Performance:** Rendered using Deko3D targeting stable gameplay.
- **Audio & Controls:** Game logic, audio, and controller input mapped and functional.

---

## 🛠️ Requirements & Setup

### For Users
1. A Nintendo Switch running **Atmosphère CFW**.
2. A legitimate dump of *The Godfather* for Original Xbox (`default.xbe` + game data files).
3. The compiled `godfather.nro` binary from the [Releases](https://github.com/CYBERPUNK20/godfather-nx/releases) page.

### Installation
1. Create a folder on your SD card: `/switch/godfather/`
2. Place `godfather.nro` inside `/switch/godfather/`
3. Extract your legal copy's game files into the `/switch/godfather/game/assets/` directory on your SD card.
4. Launch the game via HBMenu!

---

## 📜 Credits and Licensing

The runtime, hardware wrapper, and Switch graphics layer are derived from static recompilation projects including **street-nx** and upstream contributors. 

See `LICENSE`, `LICENSES.txt`, `SDK-LICENSE.txt`, and `THIRD_PARTY.txt` for applicable licenses, notices, and legal details.