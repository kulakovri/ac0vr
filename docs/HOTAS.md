# HOTAS, for the volunteers

A stick, a throttle and pedals can fly the aircraft. **Nobody has tried this on a real device yet.** It was written and
tested with SDL virtual joysticks that carry the real devices' USB IDs and axis counts. The shipped profiles are our best
guess at each device's axis and button numbers, and every one of them is marked `status = unverified` until someone with
that device says it works. That someone is you: a device line and a few sentences are enough. The headset is untested too.

## What you need to do

1. **PC mode.** Put the selector on PC *before* plugging in (T.Flight Hotas One, 4, X); choose PC / D-input / "Legacy PC" in
   the OLED menu (VelocityOne). Install the vendor's PC driver where one exists. On the T.Flight family press the Guide /
   PS button until the LED shows the **5/8 axes mode** (LED off on the One; red or green on the 4: see its manual), which keeps
   the twist, the rocker and the pedals apart. Check in Windows `joy.cpl` that every axis moves.
2. **Steam Input off** if you start the game through Steam ("Forced Off"), and no x360ce, vJoy or similar re-mapper in
   between: they make a HOTAS look like an Xbox pad. Thrustmaster TARGET or a merged virtual device changes the IDs, so
   say so in your report.
3. **Start the game** and go into a mission. The stick can be plugged in before or during the game. The log has a line per
   device (the log file: the newest `out/play_logs/zero_play_*.log`, or `zero_play.log` beside the launcher):
   ```
   joy: T.16000M GUID 030000004f0400000ab1000000000000 vid:pid 044f:b10a ver 0100 crc16 0000 type flight-stick axes 4 buttons 16 hats 1 initial-state 1111 slot 0
   joy: T.16000M: profile t16000m.ini (by VID:PID), unverified
   ```
   `profile none` means we ship nothing for your device: bind it yourself (step 6) and send us the result.
4. **Sweep everything once.** Move every axis from end to end, press every button, push every hat. Within 30 seconds the log
   gets a line that says what happened; that line tells us which number is which control:
   ```
   joy sweep: T.16000M GUID ...: axes 0:-32768..32767 1:-32768..32767 2:-32768..32767 3:-32768..32767; buttons 0 1 2 3; hats 0
   ```
5. **Set the lever's neutral.** Pause > ENHANCEMENTS > CONTROLS > SET LEVER NEUTRAL, with the lever where it should neither
   accelerate nor brake (the row is there only while a joystick is connected). See "The throttle lever" below.
6. **Fix what is wrong.** On the window (not in the headset): F4 > Controller > Joysticks and throttles. Live axis bars, per
   axis invert / slider / dead zone / curve / neutral, and a table of actions: click a binding, then move the control. Levers
   and sticks need most of their travel (a lever can rest anywhere, so a small movement does not count). Esc cancels,
   Backspace or a right-click clears. Your changes go into `build/zero/ac5_settings.ini` as a `[joystick:<GUID>]` section,
   keyed by the device and not by the USB port, and win over the shipped profile. Delete that section from the file (with the
   game closed) to go back to the shipped one.
7. **Send it back.** F4 > Copy the device report (device line + profile), plus the `joy sweep:` lines, plus your answers to
   the questions below. A profile that works is the most useful thing you can send.

## What to tell us

- Does the shipped profile's stick, twist, lever and buttons match, without changes? If not, what did you change?
- Lever: which direction is forward (raw values go up or down)? Is the neutral easy to find? Does the 8 % dead band feel
  right? Is afterburner at a sensible place? (Try `ACZ_HOTAS_AB_GATE=0.9`, see below.)
- Stick: does the aircraft answer from the first movement, and is full deflection full? (`ACZ_HOTAS_STICK=game` shows the
  game's own dead zone for comparison.) Is a `curve` above 1 better?
- `ACZ_HOTAS_THROTTLE=absolute` (an experiment, below): better or worse than CENTRED?
- Can you do a High-G turn? In the headset, is everything reachable without the keyboard?
- T.Flight Hotas One and VelocityOne with the selector on **Xbox**: how do they show up (the `joy:` line, or a `pad:` line)?
  Which axis is the throttle? We could not find this written down anywhere.
- If you know Project Wingman: is its throttle absolute (the lever is a setting that stays) or centred?
- A force-feedback stick: `ACZ_FFB=0.3`, does it feel like anything?

## How the controls map

The game was made for a DualShock 2: everything a joystick does ends as the pad's bytes, so nothing about the flight model,
replays or recordings changes. The profile merges with the pad and with Touch per action, the larger value wins. An action
your profile does not bind stays on the pad.

| Action in the profile | The game's | Notes |
|---|---|---|
| `pitch`, `roll` | left stick | One bipolar axis each: pull back positive, right positive (else `invert:1`) |
| `yaw` | L2 / R2 pressure by the sign | Twist, rocker, pedals. Or `l2` and `r2` as two buttons or a hat pair (`hat0:left`, `hat0:right`) |
| `throttle` | L1 / R1 pressure | See below |
| `accel`, `brake` | R1, L1 | Buttons always; axes in SPLIT only. `r1` and `l1` are other names for them |
| `highg` | L1 + R1 full | A lever cannot press both, so this needs a button |
| `autopilot` | L2 + R2 full | |
| `cross` `circle` `square` `triangle` | gun, missile, weapon switch, target | On the default Control Scheme. The trigger is `cross` in the shipped profiles; it also confirms in menus, and `circle` goes back |
| `select`, `start`, `r3`, `l3` | map, pause, view, L3 | In the headset a long press of `select` recentres the view; `select` + `start` held is the quit panel |
| `dpad` or `up` `right` `down` `left` | D-pad | Wingman commands, menus. `dpad = hat0` takes the whole hat |
| `look_x`, `look_y` | right stick | Mini-sticks |

The order for a stick axis: the profile's `invert`, `dz` and `curve`, then `ACZ_HOTAS_STICK`, then the Analog tab's dead zone,
sensitivity and invert, then STICK DEAD ZONE (`ACZ_PAD_STICK_REMAP`). The game's stick is 8-bit and it ignores the inner half
of each axis, so a joystick gets about 63 steps per direction however good its sensor is.

## The throttle lever

The PS2 game has no throttle setting. It has pressure on two buttons: `thr = (1.5 R1 - L1) x 1.176 / 255`, and afterburner
only at `thr = 1`. Let go and the engine returns to cruise. A lever therefore cannot be "75 % power" without help.

- **CENTRED** (default, ENHANCEMENTS > CONTROLS > THROTTLE LEVER): the lever has a neutral position (50 % unless you set it),
  with a dead band of 8 % each way (the axis's `dz`). Ahead of it: R1 pressure, growing linearly to the forward end. Behind
  it: L1 pressure, growing to the back end. Later games' presets treat a lever the same way. With a linear lever the game's
  military power comes at 51 % of the forward travel and afterburner at 55 %, because that is where a DualShock 2's R1 gets
  them: from there on it is afterburner. Setting the neutral at the back end makes the whole lever accelerate and removes the
  brake from it (then give `brake` a button).
  Measured in the simulator, default settings, a virtual lever:

  | Lever position | Pressure | The game |
  |---|---|---|
  | 0 .. 6 % | L1 217 .. 255 | power 0, air brake full |
  | 6 .. 20 % | L1 131 .. 217 | power 0, air brake opening |
  | 20 .. 42 % | L1 0 .. 131 | power 0 .. 0.40 |
  | 42 .. 58 % | none | power 0.40, what the game does with no button pressed |
  | 58 .. 80 % | R1 0 .. 131 | power 0.40 .. 1.00 (military) |
  | 81 .. 100 % | R1 140 .. 255 | afterburner |
- **SPRING**: the same with the neutral at the axis's centre: for a spring-centred rocker, paddle or mini-stick.
- **SPLIT**: `bind.accel` and `bind.brake` with their own axes: half axes (`axis3+`, `axis3-`) or twin levers (`slider:1`). The
  Warthog and X56 profiles have both levers set up for this.
- **ABSOLUTE** (`ACZ_HOTAS_THROTTLE=absolute`, not in the menu): the lever is a target for the engine power and the game's
  pressure is steered to reach it. An experiment: it changes the feel, the PS2 game never worked like this, and players of later games
  rejected levers that are "not 1:1" in the other direction. Your opinion decides whether it stays.
- `ACZ_HOTAS_AB_GATE=0.9`: the first 90 % of the forward travel spreads idle to just below afterburner, the last 10 % is
  afterburner, like a detent. Off by default; tell us which is better.
- One lever counts. With a throttle unit connected (type `throttle`), a stick's own base slider is ignored, so a T.16000M's
  slider does not fight a TWCS. A lever whose position SDL does not know yet counts as neutral until it moves. A lever counts
  only in flight: parked anywhere, it does not hold L1 or R1 down in the menus, a pause or a mission scene.
- `highg_rule = 1` in a profile: full brake on the lever with the stick pulled past half is High-G too, like the "type B" layouts of later games.
  Off by default.

## Profile files

`profiles/joystick/<device>.ini`, matched by `match` (VID:PID, comma separated), then by `match_name` (a part of the device's
name). The section `[joystick:<GUID>]` in the settings file has the same keys and wins. Two identical devices share a GUID: the
second one reads `[joystick:<GUID>:2]` if it exists, else the first one's. `;` starts a comment.

```ini
[joystick:030000004f0400000ab1000000000000]
name = T.16000M
type = stick                ; stick, throttle or pedals (a throttle's lever wins over a stick's)
highg_rule = 0
axis0 = invert:0 dz:0.03 curve:1.0
axis3 = invert:1 dz:0.08 curve:1.0 slider:0 neutral:0.5
bind.roll = axis0
bind.pitch = axis1
bind.yaw = axis2
bind.throttle = axis3
bind.cross = button0 button4   ; up to two per action
bind.l2 = hat0:left
bind.dpad = hat0
```

`axisN`: `invert` flips it, `dz` is the inner dead zone (default 0.05; a lever's dead band, default 0.08), `curve` is an
exponent on the magnitude (above 1: softer around the centre), `slider:1` reads a half-axis action from the whole travel
(0..1, for twin levers and pedals) and `neutral` is the lever's neutral. Axis, button and hat numbers count from 0 and are
SDL's: for a DirectInput device the axes it has, in the order X, Y, Z, Rx, Ry, Rz, Slider, Slider.

## Force feedback

`ACZ_FFB=0.3` (0 = off, the default; the value is the strength): a stick that reports force feedback gets the game's rumble
as a vibration. The usual HOTAS does not report any, and the log says so. An FFB stick at full strength fights your hand: start
low. The switches are environment variables: export them in Git Bash before starting the game, or put them in front of the
launcher (`ACZ_FFB=0.3 ./play_zero_desktop.sh`).

## The presets

"VID:PID from" says where the ID came from; nobody has confirmed any axis or button layout. The last column is empty until you
answer.

| File | Device | VID:PID | VID:PID from | Axes / buttons / hats (expected) | Stick, twist, lever | Verified by |
|---|---|---|---|---|---|---|
| `tflight_hotas_x.ini` | T.Flight Hotas X | 044f:b108 | memory | 5 / 12 / 1 | 0 1, 3, 2 | |
| `tflight_hotas_4.ini` | T.Flight Hotas 4 | 044f:b67b, b67c | memory | 5 / 12 / 1 | 0 1, 3, 2 | |
| `tflight_hotas_one.ini` | T.Flight Hotas One | 044f:b68d | memory | 5 / 14 / 1 | 0 1, 3, 2 | |
| `t16000m.ini` | T.16000M stick | 044f:b10a | SDL's list, a forum log | 4 / 16 / 1 | 0 1, 2, 3 | |
| `twcs.ini` | TWCS Throttle | 044f:b687 | a forum log | 8 / 14 / 1 | lever 2, paddle 5 | |
| `tfrp.ini` | T.Flight Rudder Pedals (USB) | 044f:b679 | a forum post | 3 / 0 / 0 | rudder 2 | |
| `warthog_stick.ini` | Warthog stick | 044f:0402 | SDL's list | 2 / 19 / 1 | 0 1 | |
| `warthog_throttle.ini` | Warthog throttle | 044f:0404 | SDL's list | 5 / 32 / 1 | levers 2, 3; yaw on hat 0 | |
| `x52.ini` | Saitek X52 | 06a3:075c, 0255 | a Linux kernel commit | 7 / 34 / 1 | 0 1, 5, 2 | |
| `x52_pro.ini` | Saitek X52 Pro | 06a3:0762 | a forum post | 7 / 39 / 1 | 0 1, 5, 2 | |
| `x56_stick.ini` | X56 stick | 0738:2221 | SDL's list | 5 / 17 / 2 | 0 1, 4 | |
| `x56_throttle.ini` | X56 throttle | 0738:a221 | SDL's list | 8 / 36 / 0 | levers 0, 1 | |
| `vkb_gladiator_nxt.ini` | VKB Gladiator NXT | 231d:0200, 0201 | SDL's list (0200), memory (0201) | 4 / 32 / 1 | 0 1, 3, 2 | |
| `velocityone_flightstick.ini` | VelocityOne Flightstick (PC mode) | by name only | not known | 5 / 27 / 1 | 0 1, 2, 3 | |
| `winwing_orion2.ini` | Winwing Orion 2 stick | by name only | not known | 2 / 32 / 1 | 0 1 | |
| `winwing_ursa_minor.ini` | Winwing Ursa Minor | by name only | not known | 6 / 32 / 1 | 0 1, 5, 2 | |
