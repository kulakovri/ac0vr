# ac0vr

**Version 0.1.x, an early build.** Tested on one PC and one headset so far. To play, follow the four steps below; everything from Build on is the source route, for working on the code.

A native PC recompilation of **Ace Combat Zero: The Belkan War** (PlayStation 2, US release, SLUS-21346) with virtual reality: you sit in a 3D
cockpit and look around with your head. The game's code is translated to C ahead of time on your own machine and compiled together with
a runtime that stands in for the console (graphics through Vulkan, the vector units as native code, sound, movies, memory cards,
controllers). No emulator runs while you play.

**This repository holds no game.** No code, data, pictures, sound or movies of the game, no SDK files, no prebuilt program. You need your
own disc: the recompiler reads the executable on it, and the game loads everything else from it while you play. The installer holds no game either: it builds the game on your PC from your disc. This is a free
fan project, not affiliated with or endorsed by Bandai Namco Entertainment or Sony Interactive Entertainment. It unlocks nothing and sells
nothing.

## Play it in four steps

You need your own US disc of the game. Nothing of the game is in these files: the setup builds the game on your PC from your disc.

1. **Download the installer.** On the [Releases page](../../releases/latest), take `ac0vr-0.1.x-setup.exe` (about 50 MB) and put it in a new, empty folder.
2. **Put your disc image next to it.** An ISO image of your own *Ace Combat Zero: The Belkan War* disc, US version (SLUS-21346), in the same folder. A DVD drive and a free tool such as ImgBurn make the image from the disc; a folder with the disc's files works too, the wizard asks where it is. Other regions do not work.
3. **Optional: the asset pack.** [Download it here](https://mega.nz/file/7lBzHSZR#QBPItkNZrza5PjCsPH4QmxhI62v3uO1trxn4-ORKMaI) (about 9 GB: 3D cockpits, upscaled movies and ground textures), unpack it, and put its folder `ac0vr-assets` next to the setup. This pack is not made by this project and is not part of it: it is a third-party collection that happens to fit, offered without any warranty or responsibility; use it at your own risk. Without it the game runs from its own view with the original movies, and you can add the pack later by running the setup again with the folder beside it.
4. **Run the setup.** Windows may say "Windows protected your PC" because the setup is not signed yet: click *More info*, then *Run anyway*. Click through the wizard; it finds the disc image by itself. The first start builds the game, once, in a window with a progress bar (a minute or two), then press PLAY.

From then on the desktop shortcut starts the game. Have the headset ready before PLAY (Quest: Link or Air Link running); without a headset the game runs in a window. When a new release is out, the game offers it at the start and updates itself in a few seconds. The first title screen asks, once, whether you want to share usage statistics; no is a fine answer.

**What you need:** Windows 10 or 11, 64-bit; a PC VR headset with an OpenXR runtime (a Meta Quest over Link or Air Link, or another PC headset); a strong GPU with a Vulkan driver (everything so far was tuned on an RTX 3090 Ti, and the default look uses most of it: on less, lower the resolution in ENHANCEMENTS > QUALITY); an Xbox or PlayStation gamepad.

## What it does

- The whole campaign on your PC at 4x the PS2's resolution, in stereo, at the headset's refresh rate (in-between frames at 90 or 120 Hz).
- 3D cockpits with live instruments, the game's radar and weapon data on the cockpit screens, moving stick and throttle, gloves.
- Volumetric clouds that stay put when you turn your head, weather on the canopy, head-tracked stereo sound.
- Controller glyphs for your pad, an analog-throttle controller layout, Quest Touch controllers as an option.
- An ENHANCEMENTS page in the pause menu: every improvement can be switched off.

## What you need

- **The game:** your own US disc, *Ace Combat Zero: The Belkan War*, serial SLUS-213.46, as an ISO image or as a folder with the disc's files. Other
  regions do not work: the configuration is tied to addresses in the US executable.
- **Windows** 10 or 11, 64-bit.
- **A PC VR headset through OpenXR.** A Meta Quest over Link, Air Link or Virtual Desktop, or any other headset with an OpenXR runtime. Without a headset the
  game runs in a window. See "Tested on" below.
- **A strong GPU with a Vulkan driver.** All numbers so far come from an RTX 3090 Ti, and the default look uses most of it.
- **For the source route only, all free:** [MSYS2](https://www.msys2.org) for the compiler, the libraries and Python, Git Bash or the MSYS2 shell. Disk: about 200 MB for the build, plus the disc image and the cockpits.

In the MSYS2 UCRT64 shell:

```
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
          mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
          mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-openxr-sdk mingw-w64-ucrt-x86_64-python
```

GCC has to be 15 or newer (the generated code relies on guaranteed tail calls). All the commands below run from the repository's folder, in the UCRT64 shell, or
in Git Bash after this line (change it if MSYS2 is not in `C:\msys64`):

```
export PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH
```

## Install: the details

For whoever wants more than the four steps above. The setup, `ac0vr-0.1.x-setup.exe` from this repository's Releases, goes next to an ISO image of your own US disc (or you tell it where the image or a folder with the disc's files is).

1. **Windows may stop it:** the setup is not signed yet, so SmartScreen shows "Windows protected your PC". Click *More info*, then *Run anyway*.
2. **The wizard:** the folder (`%LOCALAPPDATA%\Programs\ac0vr`, for your user only, no administrator rights), your disc (an image beside the setup is found by itself; the check reads the serial and a hash of the executable, and only the US disc passes), a desktop shortcut.
3. **The first start** builds the game from your disc, in a window with a progress bar: a minute or two on 8 threads, and only once. The result stays on your PC and the disc stays where it is: it is read while you play, never copied.

The setup holds no assets. If you have a folder `ac0vr-assets` (step 3 above), keep it next to the setup and the wizard copies it in, also on an install over an install; without one the game runs from the HUD view, with the original movies and ground textures.

After that the shortcut starts the game at once; there is no Bash, MSYS2 or emulator to deal with. Your saves, your settings and the log are in `build\zero\` of the install folder (`saves\`, `ac5_settings.ini`, `logs\`), and the uninstaller asks before it touches them.

**Updates.** At each start `ac0vr.exe` asks GitHub for the latest release (3 seconds at most; offline it just starts the game) and offers an update, with its notes. It is a small download, checked against `SHA256SUMS`, and usually a relink of a few seconds. `[update] check = 0` in `ac5_settings.ini` turns the question off. The Start menu's *ac0vr: another disc, a rebuild, updates* opens the same window by hand.

**Statistics.** The first time the game reaches the title screen it asks whether you want to share usage statistics: the hardware, the settings, the missions flown, frame times and crash summaries, once a session, to the project's PostHog (EU). Nothing is sent before you answer, nothing if you say no, and never your name, your files or your IP address. Change it any time in pause > ENHANCEMENTS > SYSTEM > SEND USAGE STATISTICS; `docs/PRIVACY.md` lists every field.

Everything from here on is the source route: for working on the code, not for playing.

## Build

**1. Take the executable from your disc.** Copy the file `SLUS_213.46` from the root of the disc or ISO to `game/SLUS_213.46` in this folder, then check it:

```
sha256sum game/SLUS_213.46
```

It has to print `34ee58ca92a6e8913aa84b6dc5af3534b878a44ca4e33173e962c6090ea243c1` (the file has 3,293,064 bytes). Anything else is a different
version: stop here.

**2. Recompile it to C** (about 10 seconds; the generated code goes to `gen_zero/` and stays on your machine):

```
PYTHONPATH=tools python -m ps2recomp game/SLUS_213.46 -o gen_zero \
    --ida-db config_zero/ida_like_zero.json --ida-seeds config_zero/seeds_zero.json \
    --symbols config_zero/sdk/sdk_symbols.json --symbols config_zero/sdk/manual_symbols.json \
    --overrides config_zero/sdk/overrides_draft.json
```

The last line says `emitted 14644 functions into 59 files`.

**3. Compile** (4 to 8 minutes on 8 threads; one generated file takes over 2 minutes by itself, so the count seems to stop now and then):

```
cmake -S . -B build/zero -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2_GENERATED_DIR=gen_zero
cmake --build build/zero -- -j 8
cp /c/msys64/ucrt64/bin/{SDL3,libopenxr_loader,libgcc_s_seh-1,libstdc++-6,libwinpthread-1}.dll build/zero/
```

This gives `build/zero/ac5.exe` and `build/zero/shaders/`. Please do not share `gen_zero/` or `build/`: they contain the game's code.

**4. Optional: recompile the vector programs.** The game uploads small programs to the PS2's VU1 while it runs. The word `native` (below) replaces the ones that matter: a flight
in mission 1 did not use the others at all in our test. Without `native` they are interpreted until you do this. Record them in a run without `native`, through the menus and a minute or two
of flying, quit, and rebuild:

```
PS2_VU_CENSUS=1 ./play_zero_vr.sh flat m1
PYTHONPATH=tools python -m vurecomp --programs out/vu_programs -o gen_zero/ps2_vu1_progs.inc
touch runtime/src/ps2_vu.c
cmake --build build/zero -- -j 8
```

In a run without `native` the log then says `vu1: N recompiled microprograms available`.

## Play

```
ACZ_DISC=/d/path/to/your_disc.iso ./play_zero_vr.sh sharp native cockpit smooth modern
```

Set up the headset first (Quest: connect Link or Air Link, or start Virtual Desktop). The launcher uses Meta's OpenXR runtime when it is installed, else the system's active one; `PS2_XR_RUNTIME=path/to/runtime.json` picks
another one (with Virtual Desktop or SteamVR on a PC that also has Meta's app, you need this). A disc in a folder works too: `ACZ_DISC=/d/path/to/folder`. The window stays black for about 20 seconds at the first start. The log goes to `zero_play.log`, the memory card to
`saves/` and your settings to `build/zero/ac5_settings.ini`.

The words after the script's name:

| Word | Meaning |
|---|---|
| `sharp` | both eyes in one pass at 4x (the mode to play in); `stick`, `stereo`, `mono`, `cinema` and `flat` (no VR, a window) are the others |
| `native` | the native renderer for models and terrain |
| `cockpit` | the 3D cockpits (below) |
| `smooth` | in-between frames at the headset's rate |
| `modern` / `classic` | the controller layout of this session |
| `m1` ... `m18` | test: boot straight into that mission (not a real save) |

Any setting can be exported before the command; `docs/ENV_REFERENCE.md` lists the switches. For a smooth picture on a Quest, set 120 Hz in the Link app: the game runs at 60 frames a second and
120 shows each frame exactly twice.

## Cockpits

The cockpit models are not in this repository: each file holds a model's meshes and textures, and most of them are GPL works of the FlightGear community. Download the **public cockpit set**
(not published yet: it will be attached to a later release on GitHub; under 1 GB, with `LICENSES.md`: sources, authors, licences, the source offer) and unpack it into `cockpits/` next to this file. `cockpits/map.txt` says which aircraft flies with
which model; `docs/aircraft_ids/README.md` lists them. 15 aircraft have their own cockpit, 19 borrow the nearest relative's, the Tornado and the YF-23A get the T-38 for now. Better ones will replace them
as models of our own are built.

Without the set, leave out the word `cockpit`: you fly from the game's own view.

The converter and the build scripts for the public set are in `tools/cockpit/` (`build_*.sh`; inputs named in each header, read from `../cockpit_src`), if you want to rebuild or change a model. Extra packs are separate and not linked here.

## Controls

An Xbox pad or a DualSense through SDL; the first pad found is the active one.

| Pad (modern layout) | In the game |
|---|---|
| Left stick | pitch and roll |
| Right stick | look (in the cockpit your head looks; the stick still works) |
| RT / LT | throttle up / down, as far as you pull them |
| RB / LB | yaw; both together: autopilot |
| A | gun |
| B | missile |
| X | weapon switch |
| Y | target |
| Menu | pause |
| View | map; held for one second: recentre the view |
| R3 | change the view (in flight) |
| View + Menu, held 1.5 s | quit panel, outside flight: A desktop, X main menu, B back |

The classic layout keeps the PS2's positions (LB/RB = L1/R1 throttle, LT/RT = L2/R2 yaw) and takes your own bindings from the settings window.

Before the game boots, a launcher in the window shows the quality settings, your GPU's suggestion and the active controller; press PLAY (Enter, Space, A / Cross or the mouse) on the monitor before you put the headset on. Its "do not show this again" comes back in F4 > Quick; `ACZ_LAUNCHER=0` skips it for one start.

On the window: F4 settings (rebinding, ENHANCEMENTS), F9 recentre, Esc twice quits. The keyboard plays too: arrows = D-pad, X, S, Z, A = the four face buttons, Q / E = L1 / R1, 1 / 3 = L2 / R2,
numpad 8 4 2 6 = left stick, Enter = Start, right Shift = Select.

In the headset: pause > ENHANCEMENTS, or View + Menu > X in the menus. `ACZ_TOUCH=pad` or `stick` turns on the Quest Touch controllers (experimental).

## Known issues

- **Tested on:** one PC (RTX 3090 Ti) with a Quest 3 over Link at 120 Hz. Air Link, Virtual Desktop, SteamVR, other headsets and other GPUs are untested. The default look uses most of a 3090 Ti: on less, lower
  the resolution in ENHANCEMENTS > QUALITY.
- Campaign saves work but the full save and reload cycle has not had a systematic test. Back up `saves/card0.ps2mc`.
- Aircraft without their own cockpit borrow a relative's: the panel will not match the aircraft.
- The HUD and menu textures are the game's originals unless you make the upscaled packs from your own disc (`tools/textures/`). Ground textures stay the game's originals, whatever GROUND TEXTURES says (its packs need tools that are not published yet), and movies play at their original 640x448.
- Explosions and smoke are the game's flat sprites at a slightly wrong depth in stereo: they can look off when close.
- Single late frames: fields of 29-55 ms show up now and then without a known cause; on CPUs with efficiency cores Windows can move the graphics threads to them under background load.
- A one-frame flash of cloud tiles was seen once at dusk over a cloud deck (not reproduced); the pause menu shows for about 4 frames over the cockpit when you press pause.
- The design eye position of some cockpits (the F-4E, the F/A-18C) is still an estimate: lean if the sight sits low.
- The program is still called `ac5.exe` and its settings file `ac5_settings.ini`, inherited from the project it grew from.

## Credits and licences

- **sal063**, [ps2recomp: Ace Combat 5](https://github.com/sal063/Ace-Combat-5-Static-recompilation): the recompiler, the runtime and the native renderer this project grew from. Its README is kept in
  `docs/README_ac5recomp_upstream.md`.
- **The FlightGear aircraft authors**: most cockpits. They are named per model in the set's `LICENSES.md`.
- Dear ImGui, Lua, stb, SDL3, the Khronos OpenXR and Vulkan projects, and Real-ESRGAN for the texture upscales of the cockpit set.

Our code is under the Apache License 2.0 (`LICENSE`, `NOTICE`). Everything else, with versions and licences: `THIRD_PARTY_NOTICES.md`. The game belongs to Bandai Namco Entertainment; please buy and keep your own copy.

## Rights holders

If you hold rights in something here and want it changed or removed, open an issue in this repository (or use GitHub's own notice process). We will answer and act quickly.
