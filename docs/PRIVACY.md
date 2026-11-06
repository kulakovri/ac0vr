# Usage statistics and your privacy

The first time you start the game, the title screen asks whether you want to share usage statistics. You answer once, and nothing
is sent before you answer. If you say no, nothing is ever sent.

This project is made by one person. Knowing which PCs and headsets play, which missions crash and how smoothly it runs decides
what gets fixed next. That is the only reason for this page.

## What is sent

Once per session, one document, plus one short document for each crash. These are all the fields. You can see the real document
in the game: **pause menu > ENHANCEMENTS > SYSTEM > SHOW WHAT IS SENT**, or press [Y] on the question.

### Each session

| Field | What it is | Example |
|---|---|---|
| `distinct_id` | A random number made when you say yes. It only ties your sessions together. It comes from nothing on your PC | `7f3c...` (32 characters) |
| `timestamp` | The day, not the time | `2026-11-04T00:00:00Z` |
| `version` | The game build's version, as the log's first line shows it | `0.1.2 (abc1234)` |
| `os_build` | The Windows version number | `10.0.26200` |
| `cpu`, `cores` | The processor's model name, and how many logical cores it has | `AMD Ryzen 7 5800X 8-Core Processor`, `16` |
| `ram_gb` | Memory, rounded to a GB | `32` |
| `gpu`, `gpu_driver`, `gpu_vram_gb` | The graphics card's name, its driver version and its memory | `NVIDIA GeForce RTX 3070`, `552.22`, `8.0` |
| `xr_runtime`, `xr_system`, `refresh_hz` | The OpenXR runtime, the headset's model, the refresh rate. Absent without a headset | `Oculus`, `Meta Quest 3`, `120.0` |
| `mode` | `vr` or `desktop` | `vr` |
| `preset`, `quality_preset` | The graphics presets in use | `high`, `HIGH` |
| `upscale`, `smooth`, `cockpit`, `touch`, `fmv_pack` | A few main settings: the resolution multiple, in-between frames, the 3D cockpit, the Touch controllers, whether the upscaled movies are there | `4`, `layered`, `on`, `auto`, `absent` |
| `controller` | The kind of controller in use: Xbox, PlayStation, other pad, Touch, HOTAS or none. Not its name | `Xbox` |
| `minutes` | How long the session lasted | `42.5` |
| `missions` | Each mission flown: its number, the aircraft's number, and the result (completed, failed or left) | `{"mission":1,"aircraft":15,"result":"completed"}` |
| `gpu_ms_p50`, `gpu_ms_p99`, `gpu_frames` | Frame times of the graphics card in flight, and how many frames they cover | `4.15`, `7.85`, `181220` |
| `gov_shed_s` | Seconds in flight that the game ran with effects turned down to keep up | `12.4` |
| `ee_ms_p50` | Frame time of the game's own code in flight | `6.35` |
| `os_lang` | The language code of Windows' display language | `de` |

Three more fields are instructions to the statistics service: `$process_person_profile: false` (make no profile of a person),
`$ip: "0.0.0.0"` (see below) and `$geoip_disable: true` (no location lookup).

### Each crash

Only if statistics are on, and only for a crash after you said yes:

| Field | What it is | Example |
|---|---|---|
| `what` | The kind of failure, with anything that could be a file name replaced by `<path>` | `exception C0000005 writing` |
| `code` | The Windows exception code | `C0000005` |
| `rva` | Where in the game's program it happened | `0012AB34` |
| `stack` | Up to eight positions in the program's own modules | `ac5.exe+0012AB34 ntdll.dll+0005A2B1` |

and `distinct_id`, the day, `version` (of the build that crashed), `os_build` and the three instructions above. The crash report on your disk (`crash/` next to
the game) holds more, such as the last log lines, which can show your folder names. **That file is never sent.** It stays with you.

## What is never sent

Your name, your Windows user name, your files or file paths, the path of your game disc, your IP address, your MAC address or any
hardware serial, a list of your programs, anything you type, or the exact time.

## The service

The documents go to [PostHog](https://posthog.com), a product analytics service, in its EU region, over HTTPS, from a background
thread of the game. PostHog is the only place they go. If the sending fails, the game keeps the document in `stats/` next to the
game and tries again at the next start.

**Your IP address.** Any connection to a server shows it your address. The game tells PostHog to store `0.0.0.0` in its place and
not to look up a location, so the stored event has neither. There is no country in the data.

## The install id, and changing your mind

`[privacy]` in `ac5_settings.ini` holds your answer, the date of the answer, and the random id. In the game, **pause menu >
ENHANCEMENTS > SYSTEM**:

- **SEND USAGE STATISTICS** turns it off or on at any time, at once. Off also deletes any document still waiting to be sent.
- **RESET INSTALL ID** replaces the random id. What was sent before stays, but it is no longer linked to your PC.

To be asked again, delete the `[privacy]` section. A build made without the project's key never asks and never sends, and the menu
says NOT AVAILABLE IN THIS BUILD.

## What you get back

The project promises to publish the aggregate numbers: the hardware share, how far people get, which missions fail most. Never
anything about one player.

## Not the same thing: the telemetry stream

TELEMETRY OUTPUT on the same page ([TELEMETRY.md](TELEMETRY.md)) sends flight data to your own motion rig or haptics on your own
PC or network. It never leaves your network and has nothing to do with these statistics.
