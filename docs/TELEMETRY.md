# The telemetry stream

The game can send its flight state to a port on your PC or your network, 60 times a second, for motion rigs, bass shakers,
force-feedback sticks and dashboards. It is off by default. Nothing here goes to the internet, and the socket only sends:
it opens no port, so Windows Firewall has nothing to ask.

## Turning it on

In the game: pause menu > ENHANCEMENTS > SYSTEM > TELEMETRY OUTPUT.

| Choice | Sends to | Value in the settings file |
|---|---|---|
| OFF | nothing | `default` |
| LOCAL | port 20777 of this PC | `127.0.0.1:20777` |
| LAN | port 20777 of every device on your network (broadcast) | `255.255.255.255:20777` |

Any other address goes into `ac5_settings.ini`, `[enhancements]`, `telemetry_output = 192.168.1.50:5300`, or the environment,
`ACZ_TELEMETRY=host:port` (the launchers apply the settings file; the environment wins over it). `off` turns it off.
With several network adapters Windows sends a broadcast through one of them only: use the receiver's own address then.

TELEMETRY FORMAT (`ACZ_TELEMETRY_FORMAT`) picks JSON or FORZA. Both rows work at once, without a restart.
The receiver example is [`tools/telemetry/recv.py`](../tools/telemetry/recv.py): `python recv.py [PORT]` prints the rate and the
last packet.

## JSON format

One UDP datagram per game frame: one line of UTF-8 JSON, under 1200 bytes (about 800 in flight). The frames the headset fills in
between send nothing. If the game drops below 60 frames a second, the stream does too. `field` tells you how far it went, and
counters mean a reader at any rate misses nothing. Outside flight (menus, a pause, a movie) one datagram per video field keeps
the state current, with the flight values `null`, or held at the last frame while paused.

A value that does not exist in this game is `null`, not zero. Ace Combat Zero has no flaps, tail hook, post-stall state or
overspeed warning for you to read: `flaps`, `hook`, `post_stall` and `overspeed` are always `null`.

| Field | Unit | Source | Meaning |
|---|---|---|---|
| `v` | | | Format version, 1 |
| `version` | | build | The game build's version, as the log's first line shows it (`0.1.2 (abc1234)`) |
| `field` | count | game | The console's video field count, about 60 a second. A step above 1 between flight packets means a field without a game frame |
| `t_ms` | ms | host | Time since the game started, on the PC's monotonic clock |
| `state` | | game | `flight`, `paused`, `menu`, `briefing`, `movie`, `cutscene` (a mission event or an intro holds the camera), `loading` |
| `aircraft` | | game | `{"id": 0..35, "name": "F-5E"}` from the last mission start, else `null` |
| `mission` | | game | The mission number, 1 = the first; `null` before the first mission |
| `speed_kt` | knots | game | The HUD's SPEED number. It is the aircraft's true speed through the world, at the world's scale of 6.656 units a metre |
| `mach` | | derived | `speed_kt` over the ISA speed of sound at `alt_ft`. The game has no temperature or Mach number of its own |
| `alt_ft` | feet | game | The HUD's ALT number (height above the world's zero, not above the ground) |
| `heading_deg` | 0..360 | game | Compass heading, increases to the right |
| `pitch_deg` | degrees | game | Nose up positive |
| `roll_deg` | degrees | game | Right wing down positive (the game's own sign is the reverse) |
| `vsi_fpm` | ft/min | derived | From the height history, smoothed |
| `g` | G | derived | Felt load along the aircraft's up axis, 1 in level flight: airspeed x pitch rate + gravity, from the flight model |
| `lat_g` | G | derived | Felt load along the right wing, from the position history (about 4 frames of smoothing, noise about 0.1 G) |
| `long_g` | G | derived | Felt load along the nose, the same way |
| `aoa_deg` | degrees | game | The flight model's angle of attack. The game's velocity follows the nose: expect only a few degrees |
| `sideslip_deg` | degrees | derived | The velocity's angle to the nose in the wing plane; positive = moving towards the right wing |
| `pitch_rate_dps` | deg/s | game | The flight model's body rate, nose up positive |
| `yaw_rate_dps` | deg/s | game | The flight model's body rate, nose right positive |
| `roll_rate_dps` | deg/s | game | The flight model's body rate, rolling right positive |
| `vel_ms` | m/s | derived | Velocity in body axes: right, up, forward |
| `throttle` | 0..1 | game | Engine power; 0.4 with no button pressed |
| `afterburner` | 0..1 | game | |
| `airbrake` | 0..1 | game | Air brake, or the wheel brake on the ground (1 while parked) |
| `stick` | -1..1 | game | Pitch (+ = pull), roll (+ = right), yaw (+ = right), after the game's dead zone |
| `missiles`, `gun`, `special` | count | game | Rounds left. `gun` read a constant 10000 in our test flights: use `gun_shots` |
| `gun_shots` | counter | game | One per the game's gun tick, every 10 fields while firing (not one per round) |
| `missiles_fired` | counter | derived | Rounds, from the count dropping |
| `specials_fired` | counter | derived | Rounds, from the count dropping |
| `hits_taken` | counter | game | Damage calls on the player's aircraft in flight. A crash into the ground is not a hit |
| `gun_firing` | bool | game | A gun tick in the last 12 fields |
| `stall`, `pull_up` | bool | game | The HUD's STALL and PULL UP warnings |
| `locked` | bool | game | An enemy seeker has locked on (the WARNING tone) |
| `missile_alert` | bool | game | A homing missile tracks you (the MISSILE tone) |
| `high_g` | bool | game | L1 + R1 held, the game's high-G manoeuvre, as the game's pad read sees them |
| `airbrake_on`, `afterburner_on` | bool | derived | The value above 0.05 |
| `gear` | bool | **modelled** | The game has no landing gear state. This is the runtime's model from its cockpit sounds: down on the runway, up after a take-off above 150 kt and 15 m, down again near the take-off runway below 170 kt and 120 m. `null` if COCKPIT SYSTEM SOUNDS is off |
| `on_ground` | bool | game | The flight model's ground contact |
| `health_pct` | 0..100 | derived | Hit points against the most seen since the mission started |

All counters count from the game's start and never go down. In a pause the event flags read `false` and the other values hold.
In a `cutscene` the values are whatever the aircraft does in it, and the flags are as they are: most tools should go neutral
for every state except `flight`.

## Forza format

`ACZ_TELEMETRY_FORMAT=forza` sends the Forza Motorsport "Data Out" Dash packet: 331 bytes, little-endian, one per game frame.
Tools that read Forza (SimHub, SimTools and haptics software, for example) should then work without a plugin: pick Forza Motorsport
in the tool and point it at port 20777. None of these tools has been tried with it yet: the packet has only been checked against
the layout below, by our own decoder. `forza7` sends the first 311 bytes instead, for tools that check for the older Forza
Motorsport 7 length (it is not on the menu).

The layout comes from Turn 10's documentation, as copied in the community tool
[richstokes/Forza-data-tools](https://github.com/richstokes/Forza-data-tools) (`FM7_packetformat.dat`, with the 2023
extension) and agreeing with the [nettrom/forza_motorsport](https://github.com/nettrom/forza_motorsport) parser (MIT). The
official page, <https://support.forza.net/hc/en-us/articles/21742934024211-Forza-Motorsport-Data-Out-Documentation>, could
not be read by the tool that wrote this (HTTP 403), so check it if a tool disagrees. Read on 2026-11-04.

**Real** = the aircraft's value. **Filled** = made from a real control so that a car tool has something to work with; a car
concept the aircraft does not have. **Zero** = always 0.

| Byte | Type | Field | Here | What it holds |
|---|---|---|---|---|
| 0 | s32 | IsRaceOn | Real | 1 in state `flight`, else 0, and then the whole packet is zero but TimestampMS, CarOrdinal and TrackOrdinal |
| 4 | u32 | TimestampMS | Real | `t_ms` |
| 8 | f32 | EngineMaxRpm | Filled | 8000 |
| 12 | f32 | EngineIdleRpm | Filled | 1000 |
| 16 | f32 | CurrentEngineRpm | Filled | 1000 + 7000 x throttle, so engine effects follow the throttle |
| 20 | f32 x3 | AccelerationX, Y, Z | Real | Felt, m/s²: right, up, forward. Y is 9.81 in level flight |
| 32 | f32 x3 | VelocityX, Y, Z | Real | `vel_ms`: right, up, forward |
| 44 | f32 x3 | AngularVelocityX, Y, Z | Real | Rad/s: pitch (nose up +), yaw (nose right +), roll (right wing down +) |
| 56 | f32 | Yaw | Real | Heading, radians, -pi..pi |
| 60 | f32 | Pitch | Real | Radians, nose up + |
| 64 | f32 | Roll | Real | Radians, right wing down + |
| 68 | f32 x4 | NormalizedSuspensionTravel | Zero | |
| 84 | f32 x4 | TireSlipRatio | Zero | |
| 100 | f32 x4 | WheelRotationSpeed | Zero | |
| 116 | s32 x4 | WheelOnRumbleStrip | Zero | |
| 132 | f32 x4 | WheelInPuddleDepth | Zero | |
| 148 | f32 x4 | SurfaceRumble | Zero | |
| 164 | f32 x4 | TireSlipAngle | Zero | |
| 180 | f32 x4 | TireCombinedSlip | Zero | |
| 196 | f32 x4 | SuspensionTravelMeters | Zero | |
| 212 | s32 | CarOrdinal | Real | The aircraft id 0..35 |
| 216 | s32 | CarClass | Zero | |
| 220 | s32 | CarPerformanceIndex | Zero | |
| 224 | s32 | DrivetrainType | Zero | |
| 228 | s32 | NumCylinders | Zero | |
| 232 | f32 x3 | PositionX, Y, Z | Real | World position in the altimeter's metres, Y up |
| 244 | f32 | Speed | Real | The HUD speed in m/s |
| 248 | f32 | Power | Filled | Throttle x 100 kW |
| 252 | f32 | Torque | Zero | |
| 256 | f32 x4 | TireTemp | Zero | |
| 272 | f32 | Boost | Filled | The afterburner, 0..1 |
| 276 | f32 | Fuel | Zero | |
| 280 | f32 | DistanceTraveled | Zero | |
| 284 | f32 x4 | BestLap, LastLap, CurrentLap, CurrentRaceTime | Zero | |
| 300 | u16 | LapNumber | Zero | |
| 302 | u8 | RacePosition | Zero | |
| 303 | u8 | Accel | Filled | Throttle x 255 |
| 304 | u8 | Brake | Filled | The air brake x 255 |
| 305 | u8 | Clutch | Zero | |
| 306 | u8 | HandBrake | Zero | |
| 307 | u8 | Gear | Zero | |
| 308 | s8 | Steer | Filled | The stick's roll x 127 |
| 309 | s8 | NormalizedDrivingLine | Zero | |
| 310 | s8 | NormalizedAIBrakeDifference | Zero | |
| 311 | f32 x4 | TireWear | Zero | Forza Motorsport (2023) only |
| 327 | s32 | TrackOrdinal | Real | The mission number. Forza Motorsport (2023) only |

The events (gun, hits, warnings) have no place in this packet: use JSON for them. Whether a tool wants a sign the other way
round is a matter for its own settings: the signs above have not been tried in SimHub or SimTools yet.

## Pointing your tools at it

There is no general telemetry hub; each tool has its own per-game inputs.

- **SimHub, SimTools, haptics software:** use the Forza format, choose a Forza Motorsport game in the tool, port 20777. Run the
  tool on this PC with LOCAL, or on another one with LAN.
- **FlyPT Mover:** if your version has a Forza input, use the Forza format. Otherwise map the JSON fields in its UDP input.
- **VPforce TelemFFB (Rhino):** it has no generic input and reads each flight simulator through that game's own scripts, so it
  needs a small adapter. `stick`, `g`, `speed_kt`, `gun_firing`, `hits_taken` and `stall` are the fields such an adapter wants.
- **Your own code:** start from `tools/telemetry/recv.py`. Only one program can bind a port on one PC: with LOCAL and two tools,
  send to one and let it forward, or use LAN.

## Cost

When it is off, the render thread does nothing for it. The game's CPU thread still does its small part: about 1-2 µs per game frame
to work out these values and three counting hooks, which is what lets the menu turn the stream on at once. When it is on, the render
thread spends about 70-100 µs per datagram, formatting and sending. `ACZ_DEBUG_TELEMETRY=N` logs the rate, the drops, the size and
these costs every N datagrams.
