<p align="center">
  <img src="docs/images/banner.svg" alt="Chaos Bleeds: PC port" width="100%">
</p>

# Buffy the Vampire Slayer: Chaos Bleeds — PC Port

This is a native Windows port of the 2003 Xbox game *Buffy the Vampire Slayer: Chaos Bleeds*. It isn't an emulator. The game's code is translated to C with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and built into an ordinary Windows program. It uses Vulkan (or Direct3D 11) for graphics, DirectSound for audio and XInput for controllers.

It also adds:

- **Sharp resolutions**: 720p, 1080p, 1440p and 4K, plus the original 4:3 sizes.
- **A steady 60 fps**, with VSync on or off, from a native Vulkan / Direct3D 11 renderer with plenty of headroom.
- **A launcher** that installs, configures and starts the game.
- **Mods**: switch them on and off with a checkbox. The mods don't change the game files on disk.
- **Story co-op**: a second player can join the campaign, in two windows or on a split screen.
- **Texture packs**: dump the game's textures and load HD replacements, as in Dolphin and PCSX2.

> [!IMPORTANT]
> **This repository has no game in it.** It holds no disc image, XBE, game data, movies or game code. You need your **own copy of the Xbox game**. Make a disc image of it (`.iso` or `.xiso`), then point the launcher at that image.

> [!NOTE]
> **This port was made with [Claude Code](https://claude.com/claude-code)**, Anthropic's AI coding assistant. Claude Code wrote most of the port's code, tools and docs, working with the project's human author, who directed, tested and played it. See [Contributors](#contributors).

---

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/title.jpg" alt="Title screen"><br><sub><b>Title screen</b>, at 1080p</sub></td>
    <td width="50%"><img src="docs/screenshots/main-menu.jpg" alt="Main menu"><br><sub><b>Main menu</b>, the game's own spell-book menu</sub></td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/gameplay-1080p.jpg" alt="Gameplay at 1080p"><br><sub><b>The Magic Box</b>, in widescreen at 1080p and 60 fps</sub></td>
    <td><img src="docs/screenshots/cutscene.jpg" alt="In-game cutscene"><br><sub><b>In-engine cutscene</b></sub></td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/multiplayer.jpg" alt="Multiplayer"><br><sub><b>Multiplayer</b>, with up to four controllers</sub></td>
    <td><img src="docs/screenshots/coop-split.jpg" alt="Split-screen story co-op"><br><sub><b>Story co-op</b>, on a split screen</sub></td>
  </tr>
</table>

---

## Contents

- [Screenshots](#screenshots)
- [What you need](#what-you-need)
- [Install and play](#install-and-play)
- [Updates](#updates)
- [Steam Deck and Linux (Proton)](#steam-deck-and-linux-proton)
- [Controls](#controls)
- [Settings](#settings)
- [Mods: what each one does](#mods-what-each-one-does)
- [Texture packs](#texture-packs)
- [Story co-op guide](#story-co-op-guide)
- [Building from source](#building-from-source)
- [Troubleshooting](#troubleshooting)
- [Contributors](#contributors)
- [Legal and credits](#legal-and-credits)

---

## What you need

| | |
|---|---|
| 💿 **The game** | A disc image of your own *Chaos Bleeds* **Xbox** disc, as an `.iso` or `.xiso`. The PS2 and GameCube versions won't work. |
| 🖥️ **PC** | Windows 10 or 11 (64-bit), with a graphics card that supports Vulkan 1.3 or Direct3D 11. A **Steam Deck** or Linux PC through Proton should work too, but it's experimental: see [Steam Deck and Linux](#steam-deck-and-linux-proton). |
| 💾 **Disk space** | About 4 GB for the installed game. |
| 🎮 **Controller** | Optional. Any XInput (Xbox-style) pad works, and so does the keyboard. Co-op needs a second controller. |
| 🎬 **FFmpeg** | Optional. It's only used once, during install, to convert the game's movies for PC. The launcher can download it for you. |

---

## Install and play

<p align="center">
  <img src="docs/images/install-flow.svg" alt="Disc image, then Launcher Install, then Settings and Mods, then Play" width="100%">
</p>

1. **Download** the latest zip from [**Releases**](https://github.com/KaikoClanworth1/buffy-chaos-bleeds-pc/releases/latest) and unzip it anywhere. It holds the launcher and the game program, with no game data.
2. **Make a disc image** of your Xbox game disc, as an `.iso` or `.xiso`.
3. **Open `Buffy Launcher.exe`** and go to the **Install** tab:
   1. Under **Disc image**, choose your `.iso` or `.xiso`.
   2. Under **Install to**, choose an empty folder.
   3. Leave **Convert the game's movies** ticked to keep the cutscene movies. If FFmpeg isn't found, click **Download FFmpeg**.
   4. Click **Install**. The launcher only reads the image and never changes it. It copies the game out (about 4 minutes) and sets up the PC version beside it.
4. **Settings tab**: pick windowed or fullscreen, the resolution, VSync and so on.
5. **Mods tab**: tick any mods you want. See the [mods table](#mods-what-each-one-does).
6. **Play tab**: press **Play**.

**Saves** are plain files, one per save, in the game folder's `SaveData\` folder (`BUFFY A.sav`, `BUFFY B.sav`, `BUFFY C.sav`). Copy them to back them up or to move them to another PC. The launcher's **Saves** tab backs them up and restores them. Saves from versions before 0.4 used the Xbox layout (`UDATA\`). They're converted the first time you start the game or open the Saves tab, and the originals are kept in `SaveBackups\`. `XboxData\` holds the emulated console's own files, and you can ignore it.

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/launcher-install.png" alt="Launcher Install tab"><br><sub><b>Install</b>: choose your disc image and a folder</sub></td>
    <td width="50%"><img src="docs/screenshots/launcher-play.png" alt="Launcher Play tab"><br><sub><b>Play</b>: start the game, check for updates</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/screenshots/launcher-saves.png" alt="Launcher Saves tab"><br><sub><b>Saves</b>: back up and restore your saves</sub></td>
    <td width="50%"><img src="docs/screenshots/launcher-advanced.png" alt="Launcher Advanced tab"><br><sub><b>Advanced</b>: export a model, edit it in Blender, make it a character mod</sub></td>
  </tr>
</table>

```mermaid
flowchart LR
    A[Your Xbox disc] -->|you rip it| B[game.iso / .xiso]
    B -->|Launcher: Install| C[Game folder]
    C --> D[buffy_chaos_bleeds.exe]
    E[mods\\ folder] -->|Launcher: Mods| D
    F[buffy_settings.ini] -->|Launcher: Settings| D
```

### Updates

From v0.3.0 the launcher updates itself. When it starts, it checks [Releases](https://github.com/KaikoClanworth1/buffy-chaos-bleeds-pc/releases) and offers any newer version. You can also press **Check now** in the **Updates** box on the **Play** tab. An update replaces the programs, shaders and the mods that come with the release. Your saves, settings and your own mods are kept. The download is checked against the SHA-256 that GitHub lists before anything is replaced. To stop the check at startup, untick **Check when the launcher starts**.

**Coming from v0.1.0 or v0.2.0:** those versions have no updater. Download the latest zip once and unzip it over your launcher's folder.

### Steam Deck and Linux (Proton)

> [!NOTE]
> **Experimental and untested so far.** The port is a Windows program. On a Steam Deck or a Linux PC it runs through **Proton**, Steam's compatibility layer. The launcher and game are made to work there, but nobody has tried them on Linux yet. If something goes wrong, please [report it](https://github.com/KaikoClanworth1/buffy-chaos-bleeds-pc/issues), with `buffy_log.txt` from the game folder. Its first lines say what the game ran on.

**Set up (Desktop Mode):**

1. Download the latest zip from [Releases](https://github.com/KaikoClanworth1/buffy-chaos-bleeds-pc/releases/latest) and unzip it, for example into `~/Games/Buffy-PC`. Copy your `.iso` onto the Deck or its SD card.
2. In Steam: **Games → Add a Non-Steam Game to My Library → Browse**. Set the file type to **All Files** and choose `Buffy Launcher.exe`.
3. In that entry's **Properties → Compatibility**, tick **Force the use of a specific Steam Play compatibility tool** and pick **Proton Experimental** (or the newest Proton).
4. Start it from Steam and use the **Install** tab as on Windows. In the file dialogs your Linux files are on drive **Z:**, for example `Z:\home\deck\Games`. **Download FFmpeg** works there too, and fetches the Windows build, which runs under Proton.

**Play in Game Mode:** add the game folder's `buffy_chaos_bleeds.exe` as another non-Steam game and force Proton on it the same way. It starts straight into the game. On a Deck, the first run is fullscreen at 1280 × 720. You can also keep using the launcher entry and press **Play** (use the trackpad or touchscreen).

**Tips:**

- **Controls:** the Deck's controls act as an Xbox controller. The launcher's **Controls** tab sets the keyboard and mouse.
- **Something draws wrong:** in the launcher's **Settings**, set **Renderer** to **Emulated Xbox GPU** and compare.
- **No movies:** the cutscene movies play through Windows Media Foundation, which depends on the Proton version. If they're skipped, try **Proton Experimental**.
- **Updates** work under Proton: the launcher unpacks them itself, without Windows' `tar.exe`, which Proton doesn't have.

---

## Controls

Up to **four** XInput controllers work, in any USB or wireless slot. The first controller is player 1, and so is the keyboard. The next controller is player 2, and so on. Each player's pad gets its own rumble.

The keys below are the defaults. You can change any of them, give an action a second key, and set mouse look and its speed on the launcher's **Controls** tab.

<p align="center"><img src="docs/screenshots/launcher-controls.png" alt="Launcher Controls tab" width="60%"></p>

| Xbox button | Controller | Keyboard |
|---|---|---|
| Start | Start | `Enter` |
| Back | Back | `Esc` |
| A / B / X / Y | A / B / X / Y | `Space` / `Backspace` / `E` / `Q` |
| Black / White | LB / RB | `Z` / `C` |
| Left / right trigger | LT / RT | `Left Shift` / `F` |
| Left stick (move) | Left stick | `W` `A` `S` `D` |
| Right stick (camera) | Right stick | `I` `J` `K` `L` |
| D-pad | D-pad | Arrow keys |

**While playing:**

- `Alt+Enter` or `F11` switches between windowed and fullscreen.
- Closing the window quits the game.
- The window title shows the frame rate.

**Bug reports:** click the **left stick** or press **F12** to save a bug report. The game creates a folder in `bug_reports\` holding:

- a screenshot of the screen
- a text report of what the game is doing: the level, the players, your settings, your mods, and exactly where the game's code is
- the log

It works even if the game has frozen. Attach the folder when you report a problem. You can open the folder from the launcher's **Play** tab, and switch the button off in **Settings**.

---

## Settings

You can change these in the launcher's **Settings** tab. In the game, **Options → PC Settings** has Resolution, VSync, Fullscreen, FPS Limit, Show FPS and Debug Overlay. Everything is saved in `buffy_settings.ini` beside the exe.

| Setting | What it does |
|---|---|
| **Windowed / Fullscreen** | Fullscreen is borderless. |
| **Resolution** | 1920×1080 is the default. 1280×720, 2560×1440 and 3840×2160 are also 16:9. The original 4:3 sizes are 640×480, 1280×960, 1920×1440 and 2560×1920. Menus and movies stay 4:3, with bars at the sides. |
| **VSync** | Waits for the monitor's refresh, so the picture doesn't tear. |
| **Renderer** | **Vulkan** (the default) and **Direct3D 11** draw the game's Direct3D calls directly through your graphics card: the same renderer, two graphics APIs. If Vulkan 1.3 isn't available, the game uses Direct3D 11 on its own. Try Direct3D 11 if Vulkan misbehaves on your system. **Emulated Xbox GPU** is the older renderer that emulates the Xbox graphics chip. Keep it as a fallback if something looks wrong, and please report it. |
| **FPS limit** | 60 (the default) or 30. The game's speed is tied to its frame rate, so it never runs above 60. |
| **Show FPS counter** | A small frame-rate counter in the top-left corner. |
| **Debug overlay** | A panel in the top-left corner with the frame rate, average and worst frame time, a graph of recent frame times, the renderer in use, the resolution and VSync, and your graphics card. |
| **Widescreen: keep the original side-to-side view** | On by default. It shows the original view with the top and bottom trimmed, which avoids pop-in and clipping at the edges of the screen. Untick it for the game's own wider view. |
| **Skip the intro movies** | Goes straight to the title screen. |
| **Invert camera left / right** | Flips the right stick's horizontal direction. |

<p align="center"><img src="docs/screenshots/launcher-settings.png" alt="Launcher Settings tab" width="60%"></p>

---

## Mods: what each one does

<p align="center">
  <img src="docs/images/mods-overlay.svg" alt="A mod folder is ticked in the launcher and laid over the game files" width="100%">
</p>

Mods live in the game folder's `mods\` folder. Tick them in the launcher's **Mods** tab and click **Apply mods**. A mod is laid *over* the game files and never written into them, so unticking it removes it completely. **None of these mods change your saves.**

<p align="center"><img src="docs/screenshots/launcher-mods.png" alt="Launcher Mods tab" width="60%"></p>

| Mod | What it does | Works in |
|---|---|---|
| 🧛 **Story co-op** *(experimental)* | A second player joins the story. Press **Start on controller 2** during a level and pick one of the **six story characters**. Both players share one inventory, and each gets their own camera, pause menu and window. See the [co-op guide](#story-co-op-guide). | Story |
| 🔄 **Player 1 Change Character** | Adds **Change Character** to player 1's pause menu. You can carry on the story as any of the six story characters. | Story |
| 💜 **Tara** *(character mod)* | Adds **Tara** to the in-level Character Select, after the story characters. She plays as Willow (same moves, spells and inventory) with her own look and Willow's voice. Her story boss model and vampire Tara are two more outfits. A character mod is a `[Character]` section in `mod.ini`: `BaseCharacter` (whose moves) and `LookRow` (whose look). | Story |
| 🕴️ **Evil Giles** *(character mod)* | Adds **Evil Giles** (Ripper, the boss) to the in-level Character Select. His boss model is put on Willow's rig (the bones converted, so every player animation drives him): her spells, stake and pickups. `Model` in `[Character]` names any model file to wear; `BaseCharacter` whose moves. Its other outfit is the Ripper, his demon form. | Story |
| 👗 **Outfits** | On the in-level Character Select, **X** changes the highlighted character's outfit (each player picks their own); a message says which. A character mod lists its outfits with `Outfits = model:skin, model:first-last` in `[Character]`. | Story |
| 🎬 **Story character outfits** | More outfits for the story characters from their cutscene and special models: Buffy 3, Willow 4, Xander 3, Faith 3, Spike 2 (`[Outfits]` in `mod.ini`). | Story |
| 🎭 **Story cast (character mods)** | One mod each, on the in-level Character Select: **Ethan Rayne**, **Anya**, **Anyanka**, **Adam**, **Civilian (Man)** and **(Woman)**, **Soldier**, **Cyborg**, **Skeleton**, **Vampire** (21 outfits), **Vampire (Woman)** (10) and **Vampire (Leader)** (7). Each is the story's own model on a story character's moves. | Story |
| 🧙 **Always play as Willow** | You play every story level as Willow, whoever the level normally uses. A level built around another character's abilities may be harder as Willow, or impossible to finish. | Story |
| ✨ **Willow: every spell from the start** | Willow has all **14 spells** from her first level instead of learning them as the story goes on. | Story and multiplayer |
| 👥 **Unlock all multiplayer characters** | Every character on the multiplayer Character Select can be picked without unlocking them in the story first. | Multiplayer |
| 🗺️ **Unlock all multiplayer arenas** | Every arena on the multiplayer Arena Select is open. | Multiplayer |

<details>
<summary><b>Making your own mod</b></summary>

```
mods\
  my-mod\
    mod.ini          name, description, optional [Patches] switches
    files\           replacement game files, laid out like the game folder
      Buffy\...
```

Here's an example `mod.ini`:

```ini
[Mod]
Name = My mod
Author = You
Version = 1.0
Description = What it does, in one or two sentences.

[Patches]
; optional switches built into the port:
; UnlockAllMultiplayerCharacters, UnlockAllMultiplayerArenas,
; UnlockAllWillowSpells, AlwaysPlayAsWillow,
; StoryCoop, StoryCoopScreens (1/2/3), Player1ChangeCharacter
```

The launcher's order decides which mod wins if two mods replace the same file. Use **Move up** and **Move down** to change the order. `mods\README.txt` in the installed game has the full details.

</details>

---

## Texture packs

Like Dolphin and PCSX2, the port can **dump** the game's textures and **load** replacements for them. You can use HD remakes, fixes, recolours or whole texture packs. Switch it on in the launcher's **Textures** tab.

<p align="center"><img src="docs/screenshots/launcher-textures.png" alt="Launcher Textures tab" width="60%"></p>

Everything lives in the game folder:

```
textures_replacement\
  dump\    the game's textures, saved as you play (when Dump is on)
  load\    your replacements (when Load is on); subfolders are fine
```

**Making a pack:**

1. Tick **Dump textures while playing** and play through the parts you want to change. Each texture is saved once, as a PNG, for example `buffy_256x256_049c2d6e895bf871_06.png`.
2. Copy the ones you want into `load\` and edit them.
   - **Any size works.** 2× or 4× the original looks sharper, and the port builds the mipmaps for you.
   - **Keep the 16-character code in the file name.** The rest of the name can change, so `…049c2d6e895bf871_06_HD.png` still matches.
   - Keep the transparent parts transparent.
3. Untick **Dump**, tick **Load custom textures**, and play.

**Sharing a pack:** zip your folder from `load\`. Other players unzip it into their own `load\` folder. Tick **Load them all when the game starts** to avoid a small stutter the first time each texture appears; it uses more memory.

> The texture code comes from the texture's contents, not where it sits in memory, so a pack works on every PC and every run.

---

## Story co-op guide

<p align="center">
  <img src="docs/images/coop-screens.svg" alt="Two windows, split screen, or one screen" width="100%">
</p>

<p align="center">
  <img src="docs/screenshots/coop-two-windows.jpg" alt="Two windows: player 1 on the left, player 2 on the right" width="100%"><br>
  <sub><b>Two Windows</b>: player 1's window (left) and player 2's (right), each with its own camera</sub>
</p>

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/coop-join.jpg" alt="Player 2 picks a character"><br><sub>Player 2 presses <b>Start</b> and picks a character</sub></td>
    <td width="50%"><img src="docs/screenshots/pause-coop.jpg" alt="Pause menu with Co-op and Change Character"><br><sub>Player 1's pause menu, with <b>Co-op</b> and <b>Change Character</b></sub></td>
  </tr>
</table>

| To… | Do this |
|---|---|
| **Join** | During a story level, press **Start** on controller 2. Pick a character with **A**, or back out with **B**. You appear next to player 1. |
| **Change character or drop out** (player 2) | Press **Start** on controller 2. Your menu has **Continue**, **Change Character** and **Drop Out**. |
| **Catch up** (player 2) | Press **Back** on controller 2 to teleport to player 1. |
| **Get back up** | If player 2 falls, they respawn beside player 1 a few seconds later. |
| **Co-op options** (player 1) | Pause, then choose **Co-op**. |

On player 1's **Co-op** page:

- **Screens**: *Two Windows*, *Split Screen* or *One Screen*. You can switch at any time. With two windows, drag player 2's window to another monitor or TV.
- **Friendly Fire**: whether the players can hurt each other.
- **Shared Inventory**: keys and items picked up by either player go into both inventories. Each player still selects their own item, and keeps their own spells and weapons.
- **Back Brings Player 2**: whether player 2's **Back** button teleports them.
- **Player 2 Respawn**: how long before player 2 gets back up.
- **Drop Out Player 2**: removes player 2 from the game.

```mermaid
sequenceDiagram
    participant P1 as Player 1 (pad 1 + keyboard)
    participant G as Game
    participant P2 as Player 2 (pad 2)
    P2->>G: Start (in a story level)
    G-->>P2: Character Select (story characters)
    P2->>G: A
    G-->>P2: spawns beside Player 1, own camera / window
    P1->>G: Pause → Co-op → Screens
    G-->>P1: Two Windows / Split Screen / One Screen
    P2->>G: Back
    G-->>P2: teleported to Player 1
```

> Co-op is **early work**. The first mission has been played through; later levels haven't been tested yet. Cutscenes and menus show on both screens.

---

## Building from source

<p align="center">
  <img src="docs/images/build-pipeline.svg" alt="XBE to xboxrecomp to C to CMake to exe" width="100%">
</p>

The translated game code is **generated on your machine from your own disc**. It isn't stored here.

**You need:**

- **Visual Studio 2019 Build Tools**: MSVC 14.29, x64.
- **CMake 3.20** or newer.
- **Git Bash**.
- **Python 3.12** with `capstone`: run `pip install capstone`.

**Steps:**

0. Clone with the submodules (the Vulkan renderer's libraries: glslang, Vulkan-Headers, volk and VMA; no Vulkan SDK needed):
   ```bash
   git clone --recursive https://github.com/KaikoClanworth1/buffy-chaos-bleeds-pc.git
   ```
   In a clone you already have, run `git submodule update --init` instead.
1. Unpack your disc into `game_files/` at the root of this repository. You need `DEFAULT.XBE`, `Buffy.map` and the `Buffy/` data folder. Either:
   - run xboxrecomp's unpacker:
     ```bash
     cd xboxrecomp && python -m tools.xiso unpack "path/to/your.iso" -o ../game_files
     ```
   - or install with a built launcher and copy the files across.
2. Generate the C code:
   ```bash
   bash port/tools/regen.sh
   ```
3. Build:
   ```bash
   cmake -S port -B port/build -A x64
   ```
   ```bash
   cmake --build port/build --config Release
   ```
4. Put a playable copy in `GAME/`, next to the build:
   ```bash
   bash port/tools/deploy.sh
   ```
   Or make a shareable folder that has no game data in it:
   ```bash
   bash port/tools/package.sh
   ```

**Repository layout:**

| Path | What's there |
|---|---|
| `port/src/` | The PC side: graphics, audio, input, movies, menus, settings, mods and co-op. |
| `port/launcher/` | The Win32 launcher (Play, Settings, Mods and Install tabs). |
| `port/mods/` | The bundled mods. |
| `port/tools/` | Regeneration, deploy and packaging scripts. |
| `xboxrecomp/` | The static recompiler and Xbox runtime. It's vendored with changes for this port. |

---

## Troubleshooting

- **The launcher says the disc image isn't Chaos Bleeds.** Only the **Xbox** version is supported, and the image must be a full disc image, not just the game partition.
- **There are no cutscene movies.** FFmpeg wasn't available during install. Get FFmpeg, then install again with **Convert the game's movies** ticked.
- **The game closed unexpectedly.** `buffy_log.txt` beside the exe records why. Please include it when you report a problem.
- **Something is drawn wrong** (missing or odd-looking graphics). In the launcher's **Settings**, set **Renderer** to **Emulated Xbox GPU** and see if it looks right there. Either way, save a bug report (click the left stick or press F12) and send it in.
- **Player 2's window is missing.** On player 1's pause menu, go to **Co-op**, then **Screens**, and choose **Two Windows** again. The window opens on the primary screen, and you can drag it from there.

---

## Contributors

| | Who | What |
|---|---|---|
| 🧑‍💻 | [**KaikoClanworth1**](https://github.com/KaikoClanworth1) | Project lead: direction, design, testing and playing. |
| 🤖 | [**Claude Code**](https://claude.com/claude-code) (Anthropic) | AI coding assistant: wrote most of the port. That includes the recompiler integration, the Vulkan and Direct3D 11 renderer and runtime fixes, audio, input, movies, the launcher, the mods system and story co-op, plus the tools and this README. |
| 🛠️ | [**sp00nznet**](https://github.com/sp00nznet) | [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), the static recompiler this port is built on. |

> **AI disclosure:** this port was developed with Claude Code. Its commits carry a `Co-Authored-By: Claude` line. Every change was run and tested on the project lead's own PC.

See [CONTRIBUTORS.md](CONTRIBUTORS.md).

---

## Legal and credits

- This is an unofficial fan project. It isn't affiliated with or endorsed by 20th Century Studios, Vivendi Universal Games, Eurocom or Microsoft. *Buffy the Vampire Slayer* and *Chaos Bleeds* belong to their respective owners.
- **No game material is included**: no disc image, XBE, data, audio, video or translated game code. You must own the game and supply your own disc image. The screenshots in `docs/screenshots/` were taken of the port running. They're used only to show the port, and they belong to the game's owners.
- **Recompiler**: [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet, under the MIT license. See `xboxrecomp/LICENSE` and `xboxrecomp/NOTICE`.
- **Disassembly**: [Capstone](https://www.capstone-engine.org/).
- **Movie conversion**: [FFmpeg](https://ffmpeg.org/). It's downloaded separately and isn't bundled.
