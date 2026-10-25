# Sea Otter API reference

The HTTP API that Sea Otter (`app/`) implements, endpoint by endpoint, as of 2026-10-24
(build local-137). Everything the panel can do, the API can do too, under the same safety
rules. The few exceptions are on purpose; see [Not over the API](#not-over-the-api-on-purpose).
[ethernet-api.md](ethernet-api.md) is the older, wider design. **This file is the
contract:** if an endpoint is not here, it doesn't exist.

## Out of the box

**The API needs no setup, and no SSH.** Install Sea Otter with the USB package and it
answers on the network straight away:

1. Connect the controller's Ethernet port to your network (or straight to a PC).
2. It has the fixed address **192.168.1.200** (mask 255.255.255.0). To change that, or to use
   DHCP, edit `network.conf` in the package before installing ([usb-install.md](usb-install.md)).
   A PC talking to it straight needs an address in the same range, e.g. 192.168.1.65.
3. Open `http://192.168.1.200:8080/` in a browser for the live panel, or call
   `http://192.168.1.200:8080/api/v1/status`.

The API is **on by default**, with everything that can't hurt anyone at the machine allowed.
Starting programs, jogging, moves, the spindle, coolant, drives-on and panel keys are **off**
until the panel's owner switches them on, one by one, in *Settings › API* (see
[Permissions](#permissions)). SSH (the `connecting/` folder) is only for working on the
controller's files, not for the API.

**Quick start** (curl; any language with HTTP works the same):
```sh
C=http://192.168.1.200:8080/api/v1
curl $C/status                                                     # where is it, what is it doing
curl -X POST --data-binary @part.nc "$C/program/upload?name=part.nc" # send a program and load it
curl -X POST -d '{"mode":"tryCut"}' $C/run/start                   # 5 s countdown, then a dry run (needs "START" on)
curl -X POST $C/run/pause                                          # feed hold
curl -X POST $C/run/start                                          # continue
curl -X POST $C/machine/stop                                       # stop anything, always works
```
`app/tools/api_smoke.py` calls every endpoint once and checks the answers; it is also a
worked example in Python.

## Permissions

*Settings › API* on the panel decides what the network may do. These switches can only be
changed on the panel, never over the API. A switched-off action answers
`403 {"error":"not_allowed","detail":"Jogging over the API is switched off on the panel (Settings > API > api.jog)","setting":"api.jog"}`.

| Setting | Default | Allows |
|---|---|---|
| `api.on` **ENABLE the REST API endpoint** | on | Anything at all. Off: every call answers 403 `api_off` |
| `api.port` | 8080 | The port (applies after a restart) |
| `api.programs` | on | `program/upload`, `program/load`, `run/arm`, `run/rewind` |
| `api.offsets` | on | `offsets/…` (incl. touch-off) |
| `api.tools` | on | `tools/…` |
| `api.settings` | on | `PUT settings`, `settings/defaults` |
| `api.overrides` | on | `overrides` |
| `api.panel` | on | `panel/toast`, `panel/instructions`, `panel/screen` |
| `api.remote_start` | **off** | `run/start`, `run/resume-from`, `move/macro`, and Start during a tool change |
| `api.jog` | **off** | `jog` |
| `api.moves` | **off** | `machine/home`, `move/gotozero` (Goto 0 / Safe Z) |
| `api.spindle` | **off** | `machine/spindle` |
| `api.coolant` | **off** | `machine/coolant` |
| `api.drives` | **off** | `machine/drives {"on":true}` (switching them **off** is always allowed) |
| `api.keys` | **off** | `key`, i.e. the clickable keys of the browser panel (it stays a live view) |

**Always allowed** (with the API on): every GET (status, offsets, tools, programs, macros,
settings, the screen) and the safety calls `run/pause`, `run/stop`, `machine/stop`, drives off.

## Basics

| | |
|---|---|
| Base URL | `http://<controller-ip>:8080/api/v1/` (port: setting `api.port`, default 8080) |
| Format | JSON request bodies (flat objects), JSON responses |
| Auth | **None.** Anyone on the network can use it. It's meant for a trusted local network. Switch it off, or limit what it may do, on the panel: *Settings › API* (see [Permissions](#permissions)) |
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
`PROBING`, `RUN`, `HOLD` (a program on hold, or a paused Goto 0: `run/start` continues either), `LIMIT`, `TOOL` (a program waits at an M6 for a tool change by
hand: jog and touch-off allowed; `run/start` twice goes back and continues, `run/stop`
stops the program). What each state accepts is in
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
  "limits": {"x": 0, "y": 0, "z": 0, "a": 0},
  "armed": "run", "remoteStart": true, "countdown": 0,
  "screen": 0, "leds": 5,
  "fpga":  {"x": 928414, "y": 503986, "z": -182402, "a": -2000},
  "steps": {"x": 928414, "y": 503986, "z": -182402, "a": -2000},
  "posChecks": 12, "posFaults": 0, "posErr": [0,0,0,0], "faultLine": -1,
  "busErrors": 0, "busRetries": 0, "busErrno": 0, "reversalsWaited": 0, "bursts": 18233,
  "perf": {"loops": 32533, "tickUs": 141, "tickUsMax": 9000, "renderUs": 50694,
           "renderUsMax": 340000, "drawUs": 26466, "presentUs": 24231, "gapMsMax": 4410,
           "motionMs": 344855, "lostMs": 0, "replanIters": 26556},
  "message": "Program started", "driverWarning": "", "macro": "", "backend": "real", "frame": 10422
}
```

| Field | Meaning |
|---|---|
| `machine` / `work` | Position in machine coordinates / in the active work offset, mm (A in degrees) |
| `program.line` | Current line (1-based) while running; the last line done otherwise |
| `overrides.feed` / `spindle` | Override in %, 0–300. `rapid` = min(feed, 100). `feedApplied` = speed actually run right now as % of the programmed feed (corners, braking), −1 when not cutting |
| `coolant` | 0 off, 1 flood (M8), 2 mist (M7), bits combined; 4 = aux (M10) |
| `limit` | The limit switch that tripped last: axis letter or `-`, and direction −1 / +1 (0 = unknown) |
| `limits` | Every axis' switch: 0 free, +1 / −1 tripped while moving that way (that direction is blocked), 2 active with the direction unknown (e.g. active at power-up). LIMIT clears when all are 0 |
| `armed` | What Start will do: `run`, `tryCut`, `simulate` |
| `remoteStart` / `countdown` | Whether API starts are allowed; seconds left of a panel countdown |
| `screen` | Panel screen: 0 Run, 1 Jog, 2 Files, 3 Work, 4 Tools, 5 Settings, 6 Macros |
| `leds` | Panel LEDs: 1 Start, 2 Pause, 4 Reset, 8 Spindle, 16 Shift, 32 Ethernet, 64 USB |
| `fpga` / `steps` | FPGA step counters / steps the app commanded. Equal at rest = no lost steps |
| `posChecks` / `posFaults` / `posErr` / `faultLine` | Position checks at rest, mismatches, last mismatch per axis (steps), program line of the last mismatch |
| `perf.lostMs` | Motion time lost to the loop being busy (should stay 0) |
| `message` | The last panel message (toast) |
| `driverWarning` | Not empty when the FPGA driver is not the one Sea Otter was tested with |
| `macro` | The macro armed or running (empty: none; `program` is then the macro) |
| `tool` | `{"loaded":3,"changeTo":4,"confirm":false}`: the tool in, the tool an M6 asks for (0 = no change going on), and whether the first `run/start` of a tool change was given (the return move is shown on the panel; the next `run/start` goes) |
| `jobPart` | Multi-part job: the work offset the feeder is on (`"G55"`), empty otherwise |
| `estimate` | `{"seconds":754,"remaining":true,"feed":120}`: the run time at the current feed override (`feed`, %), for every JOB part; `remaining` true = the time left (running, on hold, tool change), false = the whole program. −1 when not known (no program, feed 0 %). A little long on purpose (×1.05 + 1 s); tool changes by hand not included |
| `inputs` | IN01–IN18 as read, bit 0 = IN01 (1 = high). What each one is: the I/O screen, or the settings |
| `pendant` | `{"raw":1023,"held":false,"axis":-1,"mult":1,"estop":true,"wheel":0}`: register 0x3D, dead-man, axis switch (−1 OFF), multiplier, E-stop, wheel counts since start. `raw` 0x3FF = every line high: pendant unplugged, loose cable or no power (it then reads as E-stop pressed) |
| `backend` | `real` (the unit's FPGA) or `sim` (simulator) |

### GET `offsets`
The work offsets: `{"active":"G54","wcs":{"G54":{"x":375.846,"y":220.2215,"z":-162.7505,"a":0,"job":true},"G55":{…},…}}`.
The values are the machine position of each zero; `job` marks the parts of a multi-part job.

### GET `tools`
The tool table and the tool in the spindle: `{"loaded":3,"tools":[{"t":1,"dia":3.175,"len":52.41,"note":"1/8\" downcut"},…]}` (T1–T12).

### GET `programs` · `programs?source=usb`
The program files Sea Otter can load: `{"source":"local","dir":"/local","files":["bracket.nc",…]}`.
`local` is the CNCDISK network share (default), `usb` the USB stick. Extensions `.nc .ngc .tap .gcode .txt`.

### GET `program`
The loaded program: `{"loaded":true,"name":"part.nc","lines":4928,"error":"","warning":"","estimateS":1260,"box":{"min":[x,y,z],"max":[x,y,z]},"tools":[{"line":3,"tool":1},…],"resumeLine":0,"workDone":812,"macro":false}`.
`estimateS` = the whole program at 100 % feed, margin included (the live figure is `estimate` in `status`).
`box` = the cutting moves in work coordinates. `tools` = the M6 tool changes (1-based line).
`resumeLine` / `workDone` = 0 when there is none.

### GET `program/lines?from=1&count=50`
The program text: `{"name":"part.nc","from":1,"lines":["G21 G90","G0 Z5",…]}` (up to 1000 lines per call).

### GET `macros`
See [Moving](#moving).

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
| POST | `machine/spindle` | `{"on":true,"rpm":12000}` (`rpm` optional) | Spindle on/off by hand, like F6. Idle only: a program switches it with M3/M5 |
| POST | `machine/coolant` | `{"on":true}` | The flood output (M8) on/off by hand, like Shift + F6. Also while a program runs |

## Moving

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `jog` | `{"axis":"X","dist":-0.5}` | Relative jog in mm (negative = minus direction), at the jog feed. A jog past a soft/bench limit is shortened to end exactly on it (decelerating into it). Refused while running, when already at the limit that way, or into a tripped limit switch |
| POST | `move/gotozero` | – or `{"zOnly":true}` | Z up to safe height, then X Y to the active work zero. `zOnly`: Z up only (*Safe Z*). `run/pause` holds it (state HOLD), `run/start` continues, `machine/stop` ends it |
| POST | `move/macro` | `{"macro":"Park"}` | Arm a macro (by its name or its file name, see `GET macros`) and start it: same rules as `run/start` — **202** with the panel countdown, **403** if remote START is off. While it runs, `run/pause`, `run/start` and `run/stop` / `machine/stop` act on it; afterwards the loaded program is back. **409** e.g. `"Park: home first"` for a `(needs: homed)` macro; **400** if the file has an error (`detail`) |
| GET | `macros` | – | The macro files found: `{"armed":"","macros":[{"name":"Park","info":"…","file":"/ddcsv4/macro-park.nc","needsHomed":true,"source":"package"},…]}` — `source` `package` (app folder) or `share` (CNCDISK `macros` folder) |

## Work offsets

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `offsets/touch-off` | `{"axis":"X","dir":1,"diameter":12}` | Touch-off at the current position, like Shift + axis key on the panel: axis = −dir·Ø/2 (`diameter` optional; it also becomes the panel's edge Ø). `dir` −1 / +1 |
| POST | `offsets/active` | `{"wcs":"G55"}` | Make G54…G59 the active work offset |
| POST | `offsets/G54` … `offsets/G59` | `{"x":120.5,"y":80,"z":-30.2,"job":true}` | Set offset values (machine position of that zero; any subset) and the multi-part "job" flag. Several JOB parts: Start cuts each tool on every part before the next tool change |

## Tools

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `tools/T1` … `tools/T12` | `{"dia":6,"len":58.33,"note":"6 mm flat 3F"}` (any subset) | Edit the tool table |
| POST | `tools/loaded` | `{"tool":3}` | Tell Sea Otter which tool is in the spindle now (0 = none). An M6 for that tool then doesn't stop. Not while a program runs |

A tool change in a program (M6) is handled as set in *Settings › Tool change*: by hand (state
`TOOL`), by a macro, or ignored. By hand: jog and `offsets/touch-off` work during it, then
`run/start` twice (the first shows the way back on the panel, the second goes).

## Programs

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `program/upload?name=part.nc` | the raw G-code text (not JSON) | Saves the file in `/local` (the CNCDISK share) and loads it. 400 with `detail` and `line` if it doesn't parse |
| POST | `program/load` | `{"name":"part.nc"}` · `{"name":"part.nc","source":"usb"}` | Load a file that is already on the controller (`GET programs` lists them): the CNCDISK share (default) or the USB stick. 404 if the file isn't there, 400 with `detail` if it doesn't parse. 4 s for 64 000 lines; the panel freezes for that time |

Example upload with curl:
```sh
curl -X POST --data-binary @part.nc "http://192.168.1.200:8080/api/v1/program/upload?name=part.nc"
```

## Running

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `run/start` | `{"mode":"run"}` · `"tryCut"` · `"simulate"` (optional) | Start the loaded program. From IDLE: **202**, the panel counts down `api.start_delay` s first (Reset on the panel cancels). From HOLD: resumes at once. From TOOL (a tool change): the first call only shows the return move on the panel (`tool.confirm` becomes true), the second goes back in a straight line at `tool.return_pct` of rapid and continues. **403** if remote START is off on the panel. **409** if the program leaves the soft/bench limits on any work offset it would run on (`detail` names axis, side and distance; Simulate is allowed) |
| POST | `run/arm` | `{"mode":"tryCut"}` | What Start will do, without starting: `run`, `tryCut` (spindle off, Z at the clearance level) or `simulate` (nothing moves). Like the panel's F5 |
| POST | `run/pause` | – | Feed hold (RUN → HOLD); also holds a Goto 0 |
| POST | `run/rewind` | – | Forget the resume point and the work done: the next Start begins at line 1 |
| POST | `run/stop` | – | Stop the program (409 when nothing runs; `machine/stop` always works) |
| POST | `run/resume-from` | `{"line":846,"mode":"run"}` | Restore the modal state of that line (1-based) and continue from it: Z up, XY across, down. Same countdown / 403 rules as `run/start` |
| POST | `overrides` | `{"feed":110,"spindle":95}` | Set feed / spindle override in %, 0–300 (any subset). Answers `{"feed":110,"rapid":100,"spindle":95}` |

## Panel

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `panel/screen` | `{"screen":"files"}` | Show a screen: `run`, `jog`, `files`, `work`, `tools`, `settings`, `macros`, `io` (Jog is refused while something moves) |
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

## Complete control: the panel and its API calls

| On the panel | Over the API |
|---|---|
| Start / Pause / Reset | `run/start` · `run/pause` · `run/stop` or `machine/stop` |
| Arm Run / TryCut / Simulate (F5) | `run/arm`, or `mode` in `run/start` |
| Resume from a line · Rewind | `run/resume-from` · `run/rewind` |
| Feed / spindle overrides | `overrides` |
| Spindle (F6) · Coolant (Shift + F6) | `machine/spindle` · `machine/coolant` |
| Drives on/off · Home | `machine/drives` · `machine/home` |
| Jog · Goto 0 · Safe Z | `jog` · `move/gotozero` · `move/gotozero {"zOnly":true}` |
| Touch-off · active WCS · offsets table · JOB marks | `offsets/touch-off` · `offsets/active` · `offsets/G54…G59` · `GET offsets` |
| Tools table · tool in the spindle | `tools/T1…T12` · `tools/loaded` · `GET tools` |
| Files: list, load | `GET programs` · `program/load` · `program/upload` (send a new one) |
| Program listing | `GET program` · `GET program/lines` |
| Macros: pick and run | `GET macros` · `move/macro` |
| Tool change: back to the program | `run/start` (twice) |
| Settings: every value · defaults | `GET`/`PUT settings` · `settings/defaults` (except `api.*`, panel only) |
| I/O overview | `GET status` (`inputs`, `pendant`, outputs via `spindle` / `coolant`), `panel/screen {"screen":"io"}` |
| Screens · any key | `panel/screen` · `key` |
| Messages, checklists for the operator | `panel/toast` · `panel/instructions` |
| The screen itself | `GET /frame` (raw image), `/` (live browser panel) |

## Not over the API (on purpose)

| What | Why |
|---|---|
| Releasing an **E-stop** | An E-stop is a physical button; it is released at the machine. The API reports it (`state` `ESTOP`) |
| **Continuous jog** (jog while a key is held) | Over a network a lost "key released" message would leave the axis running. `jog` moves a fixed distance instead |
| Changing any **API permission** (`api.*`) | They decide who may control the machine, so only the panel can change them (403 `read_only`) |
| The **pendant** | It is a hand-held device at the machine |
| Skipping the start **countdown** | A remote start gives the operator `api.start_delay` seconds (default 5) to see it coming and press Reset. Only the panel can set that to 0 |

## Simulator only

| Method | Path | Body | Does |
|---|---|---|---|
| POST | `sim` | `{"held":true,"axis":0,"mult":10,"wheel":4,"pendantEstop":false,"estop":false,"limit":"X"}` (any subset) | Pendant, E-stops and limit switches of the simulated machine. On the real unit: 409 `not_simulated` |

---

## Settings keys

`PUT settings` (or POST) with `{"key": value, …}` changes any subset, e.g.
`{"jog.feed": 1500, "motion.graceful_stop": true}`. Refused (409) while the machine is busy.
`axis.*` keys re-initialise the FPGA and are also refused (409) while a jog is still
moving.
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
| `api.on` | API | ENABLE the REST API endpoint (HTTP) *(panel only)* |  | on/off |  |
| `api.port` | API | Port, restart to apply *(panel only)* |  | whole number, 1 to 65535 |  |
| `api.programs` · `api.offsets` · `api.tools` · `api.settings` · `api.overrides` · `api.panel` | API | Allow … (see [Permissions](#permissions)) *(panel only)* |  | on/off, default on |  |
| `api.agent_msgs` | API | Show agent checklists on the Run screen *(panel only)* |  | on/off |  |
| `api.remote_start` | API | Allow: START programs, macros, tool-change return *(panel only)* |  | on/off, default off |  |
| `api.start_delay` | API | Remote start countdown *(panel only)* | s | whole number, 0 to 30 |  |
| `api.jog` · `api.moves` · `api.spindle` · `api.coolant` · `api.drives` · `api.keys` | API | Allow … (see [Permissions](#permissions)) *(panel only)* |  | on/off, default off |  |
| `axis.<X|Y|Z|A>.steps` | Axes | steps/mm |  | number, 1 to 100000 | yes |
| `axis.<X|Y|Z|A>.max_vel` | Axes | max speed | mm/min | number, 1 to 30000 | yes |
| `axis.<X|Y|Z|A>.accel` | Axes | acceleration | mm/s² | number, 1 to 10000 | yes |
| `axis.<X|Y|Z|A>.jerk` | Axes | jerk (S-curve) | mm/s³ | number, 10 to 1000000 | yes |
| `axis.<X|Y|Z|A>.corner_jump` | Axes | corner speed jump | mm/min | number, 0 to 6000 | yes |
| `axis.<X|Y|Z|A>.dir_invert` | Axes | DIR invert |  | on/off | yes |
| `axis.<X|Y|Z|A>.soft_min` | Homing & limits | soft limit − | mm | number, -9999 to 9999 | yes |
| `axis.<X|Y|Z|A>.soft_max` | Homing & limits | soft limit + | mm | number, -9999 to 9999 | yes |
| `axis.<X|Y|Z|A>.bench_min` | Homing & limits | bench limit − | mm | number, -99999 to 99999 | yes |
| `axis.<X|Y|Z|A>.bench_max` | Homing & limits | bench limit + | mm | number, -99999 to 99999 | yes |
| `axis.<X|Y|Z|A>.limit_port` | Homing & limits | limit/home switch input (IN, 0 = none) |  | whole number, 0 to 18 | yes |
| `axis.<X|Y|Z|A>.home_dir` | Homing & limits | home direction |  | choice index 0 to 1 | yes |
| `axis.<X|Y|Z|A>.home_pos` | Homing & limits | position at switch | mm | number, -9999 to 9999 | yes |
| `axis.<X|Y|Z|A>.home_backoff` | Homing & limits | back-off after home | mm | number, 0 to 500 | yes |
| `home.order` | Homing & limits | Home order |  | choice index 0 to 3 | yes |
| `limits.soft` | Homing & limits | Soft limits (when homed) |  | on/off | yes |
| `home.retain` | Homing & limits | Keep the homed state after a restart / power cut (only if the machine was at rest; off by default) |  | on/off | yes |
| `limits.bench` | Homing & limits | Bench limits (work box, also un-homed) |  | on/off | yes |
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
| `tool.mode` | Tool change | On M6 |  | 0 Pause: change by hand · 1 Run a tool-change macro · 2 Ignore M6 | yes |
| `job.order` | Tool change | Several JOB parts: cut |  | 0 One tool at a time (all parts, then change) · 1 One part at a time (all tools on it) | yes |
| `tool.where` | Tool change | Change the tool at |  | 0 Z up to safe height, stay here · 1 Tool-change position (homed) | yes |
| `tool.return_pct` | Tool change | Return to the program at (% of rapid) | % | number, 5 to 100 | yes |
| `macro.tc_x` | Tool change | Tool change X (machine) | mm | number, -9999 to 9999 | yes |
| `macro.tc_y` | Tool change | Tool change Y (machine) | mm | number, -9999 to 9999 | yes |
| `macro.tc_z` | Tool change | Tool change Z (machine) | mm | number, -9999 to 9999 | yes |
| `macro.warm_rpm` | Macros | Warm-up spindle speed | rpm | number, 0 to 60000 | yes |
| `macro.warm_s` | Macros | Warm-up time | s | number, 1 to 3600 | yes |

`axis.<X|Y|Z|A>.…` stands for the four keys `axis.X.…`, `axis.Y.…`, `axis.Z.…`, `axis.A.…` (capital letter).
