# Pandora API reference

The HTTP API that Pandora (`app/`) actually implements, endpoint by endpoint, as of build
local-74 (2026-10-18). [ethernet-api.md](ethernet-api.md) is the wider design: it also
lists endpoints that are planned but not built yet. **This file is the contract:** if an
endpoint is not here, it doesn't exist yet.

## Basics

| | |
|---|---|
| Base URL | `http://<controller-ip>:8080/api/v1/` (port: setting `api.port`, default 8080) |
| Format | JSON request bodies (flat objects), JSON responses |
| Auth | **None.** Anyone on the network can use it. It's meant for a trusted local network. Switch it off on the panel: *Settings › Network & API › API* |
| Switched off | Every `/api/v1/` call answers `403 {"error":"api_off"}` |
| Same rules as the panel | Every call that moves or changes the machine goes through the same state machine as a panel key. A call the panel would refuse is refused here with the same reason |

Also served on the same port (not under `/api/v1/`):

| Method | Path | Returns |
|---|---|---|
| GET | `/` or `/panel` | The browser panel: the live LCD image and clickable keys |
| GET | `/frame` | The current screen: raw RGB565, 1024 × 600, little-endian (1 228 800 bytes). Headers `X-Frame` (frame counter) and `X-Leds` (LED bits) |

## Status codes

| Code | Meaning | Body |
|---|---|---|
| 200 | Done | `{"ok":true,"state":"IDLE"}` or the data asked for |
| 202 | Accepted, will happen after the panel countdown (`run/start`) | `{"ok":true,"countdown":5,"detail":"…"}` |
| 400 | Bad request (missing field, G-code error) | `{"error":"…","detail":"…","line":n}` |
| 403 | API off, remote START disabled, or a panel-only setting | `{"error":"api_off" \| "remote_start_disabled" \| "read_only", …}` |
| 404 | Unknown endpoint, or a GET to a POST-only endpoint | `{"error":"unknown endpoint"}` or `not found` |
| 409 | The machine's state refuses it right now | `{"error":"state","state":"RUN","detail":"Program running - directional controls are locked …"}` |

## Machine states

`state` is one of: `ESTOP`, `OFF` (drives off), `IDLE`, `MOVE` (goto / macro), `HOMING`,
`PROBING`, `RUN`, `HOLD`, `LIMIT`. What each state accepts is in
[operator-procedures.md](operator-procedures.md) § state machine.

---

## Reading

### GET `status`
Everything in one snapshot. Poll it (e.g. every 0.5–1 s).

```json
{
  "state": "RUN", "homed": true,
  "machine": {"x": 464.2070, "y": 251.9930, "z": -91.2010, "a": -1.0000},
  "work":    {"x": 0.0000,   "y": 114.2900, "z": 5.0000,   "a": -1.0000},
  "wcs": "G54",
  "program": {"name": "part.nc", "loaded": true, "lines": 4928, "line": 812},
  "overrides": {"feed": 100, "rapid": 100, "spindle": 100, "feedApplied": 87.5},
  "spindle": {"on": true, "rpm": 12000}, "coolant": 1,
  "limit": {"axis": "-", "dir": 0},
  "armed": "run", "remoteStart": true, "countdown": 0,
  "screen": 0, "leds": 5,
  "fpga":  {"x": 928414, "y": 503986, "z": -182402, "a": -2000},
  "steps": {"x": 928414, "y": 503986, "z": -182402, "a": -2000},
  "posChecks": 12, "posFaults": 0, "posErr": [0,0,0,0], "faultLine": -1,
  "busErrors": 0, "busRetries": 0, "busErrno": 0, "reversalsWaited": 0, "bursts": 18233,
  "perf": {"loops": 32533, "tickUs": 141, "tickUsMax": 9000, "renderUs": 50694,
           "renderUsMax": 340000, "drawUs": 26466, "presentUs": 24231, "gapMsMax": 4410,
           "motionMs": 344855, "lostMs": 0, "replanIters": 26556},
  "message": "Program started", "backend": "real", "frame": 10422
}
```

| Field | Meaning |
|---|---|
| `machine` / `work` | Position in machine coordinates / in the active work offset, mm (A in degrees) |
| `program.line` | Current line (1-based) while running; the last line done otherwise |
| `overrides.feed` / `spindle` | Override in %, 0–300. `rapid` = min(feed, 100). `feedApplied` = speed actually run right now as % of the programmed feed (corners, braking), −1 when not cutting |
| `coolant` | 0 off, 1 flood (M8), 2 mist (M7), bits combined; 4 = aux (M10) |
| `limit` | Tripped limit: axis letter or `-`, and direction −1 / +1 (0 = unknown) |
| `armed` | What Start will do: `run`, `tryCut`, `simulate` |
| `remoteStart` / `countdown` | Whether API starts are allowed; seconds left of a panel countdown |
| `screen` | Panel screen: 0 Run, 1 Jog, 2 Files, 3 Work, 4 Tools, 5 Settings, 6 Macros |
| `leds` | Panel LEDs: 1 Start, 2 Pause, 4 Reset, 8 Spindle, 16 Shift, 32 Ethernet, 64 USB |
| `fpga` / `steps` | FPGA step counters / steps the app commanded. Equal at rest = no lost steps |
| `posChecks` / `posFaults` / `posErr` / `faultLine` | Position checks at rest, mismatches, last mismatch per axis (steps), program line of the last mismatch |
| `perf.lostMs` | Motion time lost to the loop being busy (should stay 0) |
| `message` | The last panel message (toast) |
| `backend` | `real` (the unit's FPGA) or `sim` (simulator) |

### GET `settings`
All settings as `{"key": value, …}` (on/off as 0/1, choices as their index). The keys are
listed in [Settings keys](#settings-keys) below.

---

## Machine

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `machine/stop` | – | Stops whatever moves (program, move, jog, homing) and cancels a start countdown. Always answers 200: `{"ok":true,"state":"IDLE","fpgaAtStop":{"x":…}}`, with the FPGA counters read at the moment of the stop. With `motion.graceful_stop` on (default) the machine brakes along the path (no lost steps, ≈ 1 mm run-on at full speed) |
| POST | `machine/drives` | `{"on":true}` / `{"on":false}` | Drives on / off. Off is always immediate |
| POST | `machine/home` | – | Home all axes in the configured order (Reset or `machine/stop` stops it) |

## Moving

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `jog` | `{"axis":"X","dist":-0.5}` | Relative jog in mm (negative = minus direction), at the jog feed. Refused while running, at a soft limit, or into a tripped limit switch |
| POST | `move/gotozero` | – | Z up to safe height, then X Y to the active work zero |
| POST | `move/macro` | `{"macro":"safez"}` | Run a macro: `safez`, `workzero`, `park`, `toolchange` (machine positions from `macro.*`, need a homed machine), `warmup` (spindle at `macro.warm_rpm` for `macro.warm_s`). Also by number 0–4 |

## Work offsets

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `offsets/touch-off` | `{"axis":"X","dir":1,"diameter":12}` | Touch-off at the current position, like Shift + axis key on the panel: axis = −dir·Ø/2 (`diameter` optional; it also becomes the panel's edge Ø). `dir` −1 / +1 |
| POST | `offsets/active` | `{"wcs":"G55"}` | Make G54…G59 the active work offset |
| POST | `offsets/G54` … `offsets/G59` | `{"x":120.5,"y":80,"z":-30.2,"job":true}` | Set offset values (machine position of that zero; any subset) and the multi-part "job" flag |

## Programs

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `program/upload?name=part.nc` | the raw G-code text (not JSON) | Saves the file in `/local` (the CNCDISK share) and loads it. 400 with `detail` and `line` if it doesn't parse |
| POST | `program/load` | `{"name":"part.nc"}` | Load a file that is already in `/local`. 4 s for 64 000 lines; the panel freezes for that time |

Example upload with curl:
```sh
curl -X POST --data-binary @part.nc "http://192.168.1.200:8080/api/v1/program/upload?name=part.nc"
```

## Running

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `run/start` | `{"mode":"run"}` · `"tryCut"` · `"simulate"` (optional) | Start the loaded program. From IDLE: **202**, the panel counts down `api.start_delay` s first (Reset on the panel cancels). From HOLD: resumes at once. **403** if remote START is off on the panel |
| POST | `run/pause` | – | Feed hold (RUN → HOLD) |
| POST | `run/stop` | – | Stop the program (409 when nothing runs; `machine/stop` always works) |
| POST | `run/resume-from` | `{"line":846,"mode":"run"}` | Restore the modal state of that line (1-based) and continue from it: Z up, XY across, down. Same countdown / 403 rules as `run/start` |
| POST | `overrides` | `{"feed":110,"spindle":95}` | Set feed / spindle override in %, 0–300 (any subset). Answers `{"feed":110,"rapid":100,"spindle":95}` |

## Panel

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `panel/toast` | `{"text":"Coffee break - back at 3"}` | Show a message on the panel (max 63 characters) |
| POST | `panel/instructions` | see below | Show a checklist card on the Run screen (an agent preparing a job) |
| DELETE | `panel/instructions` | – | Remove the card |
| POST | `key` | `{"key":"F1","down":true}` then `{"key":"F1","down":false}` | Press / release a panel key. Names: `F1`…`F7`, `TAB`, `SEL`, `SHIFT`, `MOD`, `MODE`, `CANCEL`, `YP`, `YM`, `XP`, `XM`, `ENTER`. A remote F1 on the Run screen counts as a remote start (403 if those are off) |

`panel/instructions` body:
```json
{"title": "Bracket, 2 parts", "who": "CAM agent",
 "steps": [
   {"text": "Home the machine", "check": "homed"},
   {"text": "Load bracket.nc", "check": "loaded:bracket.nc"},
   {"text": "Clamp the stock", "check": "operator"},
   {"text": "Touch off X Y Z on G54", "check": "touched:XYZ"},
   {"text": "Start", "check": "running"}]}
```
Each step is ticked off by itself when its `check` becomes true: `homed`,
`loaded:<file>`, `touched:<axes>` (touch-off done on those axes), `running`. `operator`
steps are ticked by the operator on the panel. Up to 8 steps.

## Simulator only

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `sim` | `{"held":true,"axis":0,"mult":10,"wheel":4,"pendantEstop":false,"estop":false,"limit":"X"}` (any subset) | Pendant, E-stops and limit switches of the simulated machine. On the real unit: 409 `not_simulated` |

---

## Settings keys

`PUT settings` (or POST) with `{"key": value, …}` changes any subset, e.g.
`{"jog.feed": 1500, "motion.graceful_stop": true}`. Refused (409) while the machine is busy.
Keys marked *panel only* answer 403: they decide who may control the machine, so they can
only be changed on the panel. Answer: `{"ok":true,"changed":2}`.

﻿| Key | Group | Setting | Unit | Type / range | Idle only |
|---|---|---|---|---|---|
| `display.pos_dec` | Display | Position decimals (X Y Z A) |  | choice index 1 to 3 |  |
| `display.tool_dec` | Display | Tool size decimals (Ø, length) |  | choice index 1 to 3 |  |
| `display.view` | Display | Default toolpath view |  | choice index 0 to 2 |  |
| `jog.edge_dia` | Jog & touch-off | Edge finder / tool Ø | mm | number, 0 to 50 |  |
| `jog.feed` | Jog & touch-off | Jog feed | mm/min | number, 1 to 6000 | yes |
| `jog.safe_z` | Jog & touch-off | Safe Z above work zero | mm | number, 0 to 200 | yes |
| `motion.graceful_stop` | Homing & limits | Controlled stop on Reset, E-stop, limit (no lost steps) |  | on/off | yes |
| `planner.lookahead` | Axes | Planner lookahead (segments) |  | whole number, 4 to 400 | yes |
| `planner.junction_dev` | Axes | Corner tolerance | mm | number, 0 to 1 | yes |
| `gcode.default_feed` | Jog & touch-off | Feed when a program sets none | mm/min | number, 1 to 20000 | yes |
| `pendant.on` | Pendant | MPG pendant fitted (optional) |  | on/off |  |
| `pendant.quick_ms` | Pendant | Quick press = Enter, shorter than | ms | whole number, 100 to 1500 |  |
| `pendant.wheel_div` | Pendant | Counts per wheel detent |  | whole number, 1 to 8 |  |
| `pendant.wheel_reverse` | Pendant | Reverse wheel direction |  | on/off |  |
| `pendant.estop` | Pendant | Pendant E-stop enabled |  | on/off | yes |
| `api.on` | Network & API | API (HTTP, port below) *(panel only, read-only over the API)* |  | on/off |  |
| `api.port` | Network & API | API port *(panel only, read-only over the API)* |  | whole number, 1 to 65535 |  |
| `api.remote_start` | Network & API | Allow remote START (agents, scripts) *(panel only, read-only over the API)* |  | on/off |  |
| `api.start_delay` | Network & API | Remote start countdown *(panel only, read-only over the API)* | s | whole number, 0 to 30 |  |
| `api.agent_msgs` | Network & API | Show agent instructions on panel |  | on/off |  |
| `axis.<X|Y|Z|A>..steps` | Axes | steps/mm |  | number, 1 to 100000 | yes |
| `axis.<X|Y|Z|A>..max_vel` | Axes | max speed | mm/min | number, 1 to 30000 | yes |
| `axis.<X|Y|Z|A>..accel` | Axes | acceleration | mm/s² | number, 1 to 10000 | yes |
| `axis.<X|Y|Z|A>..jerk` | Axes | jerk (S-curve) | mm/s³ | number, 10 to 1000000 | yes |
| `axis.<X|Y|Z|A>..corner_jump` | Axes | corner speed jump | mm/min | number, 0 to 6000 | yes |
| `axis.<X|Y|Z|A>..dir_invert` | Axes | DIR invert |  | on/off | yes |
| `axis.<X|Y|Z|A>..soft_min` | Homing & limits | soft limit − | mm | number, -9999 to 9999 | yes |
| `axis.<X|Y|Z|A>..soft_max` | Homing & limits | soft limit + | mm | number, -9999 to 9999 | yes |
| `axis.<X|Y|Z|A>..home_dir` | Homing & limits | home direction |  | choice index 0 to 1 | yes |
| `axis.<X|Y|Z|A>..home_pos` | Homing & limits | position at switch | mm | number, -9999 to 9999 | yes |
| `axis.<X|Y|Z|A>..home_backoff` | Homing & limits | back-off after home | mm | number, 0 to 500 | yes |
| `home.order` | Homing & limits | Home order |  | choice index 0 to 3 | yes |
| `limits.soft` | Homing & limits | Soft limits (when homed) |  | on/off | yes |
| `limits.level_high` | Homing & limits | Limit/home active when open (NC) |  | on/off | yes |
| `estop.port` | Homing & limits | External E-stop input (0 = none) | IN | whole number, 0 to 18 | yes |
| `estop.level_high` | Homing & limits | E-stop active when open (NC) |  | on/off | yes |
| `spindle.max` | Spindle & outputs | Spindle max RPM (10 V) | rpm | number, 100 to 60000 | yes |
| `spindle.default` | Spindle & outputs | Spindle RPM when none given | rpm | number, 0 to 60000 | yes |
| `spindle.delay` | Spindle & outputs | Spin-up wait | s | number, 0 to 30 | yes |
| `out.spindle` | Spindle & outputs | M3 output (OUT n, 0 = none) |  | whole number, 0 to 3 | yes |
| `out.flood` | Spindle & outputs | M8 output |  | whole number, 0 to 3 | yes |
| `out.aux` | Spindle & outputs | M10 output |  | whole number, 0 to 3 | yes |
| `macro.park_x` | Macros | Park X (machine) | mm | number, -9999 to 9999 | yes |
| `macro.park_y` | Macros | Park Y (machine) | mm | number, -9999 to 9999 | yes |
| `macro.park_z` | Macros | Park Z (machine) | mm | number, -9999 to 9999 | yes |
| `macro.tc_x` | Macros | Tool change X (machine) | mm | number, -9999 to 9999 | yes |
| `macro.tc_y` | Macros | Tool change Y (machine) | mm | number, -9999 to 9999 | yes |
| `macro.tc_z` | Macros | Tool change Z (machine) | mm | number, -9999 to 9999 | yes |
| `macro.warm_rpm` | Macros | Warm-up spindle speed | rpm | number, 0 to 60000 | yes |
| `macro.warm_s` | Macros | Warm-up time | s | number, 1 to 3600 | yes |

`axis.<X|Y|Z|A>.…` stands for the four keys `axis.X.…`, `axis.Y.…`, `axis.Z.…`, `axis.A.…` (capital letter).
