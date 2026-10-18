# Ethernet API — full remote control of the CNC

_2026-10-17. Design spec, not yet implemented. The replacement app must be **fully
controllable over Ethernet**: every step needed to operate the machine — power or stop
the drives, home the axes, jog and move, probe, upload and load a program, start /
pause / stop a `.nc` program, resume from a line, overrides, spindle, coolant, offsets,
tools, I/O and settings — is an API call. Operator-side design:
[operator-ux.md](operator-ux.md); build: [architecture.md](architecture.md)._

## Principles

- **Parity with the panel.** The panel UI calls the same command layer the API exposes.
  If the panel can do it, the API can do it, with the same rules (limit policy, states).
- **One controller, many observers.** A client must hold the **control lease** to send
  commands that move or change things. Any number of clients can observe. The panel
  takes the lease back with any key press; a remote client takes it with
  `POST /lease`. The Ethernet LED shows who has it.
- **Physical stop always wins.** The E-stop and the panel `Reset` key act regardless of
  who holds the lease. `POST /machine/stop` is accepted from **any** authenticated
  client, lease or not.
- **Not overly protective.** Commands are refused only when the machine can't do them
  safely right now (E-stop active, drives off, moving into a tripped limit). A refusal
  says why and what would unblock it.
- **Same state machine as the panel.** Every call goes through the one dispatcher
  described in [operator-procedures.md](operator-procedures.md) § Machine state
  machine; a call refused there is refused here (`409 {"error":"state","state":"RUN",...}`).
  While a program runs or holds, all motion calls (jog, move, home, probe, zero) get 409.
- **Remote START is on by default** (changed 2026-10-18; it was off). Whoever distrusts remote agents switches it off on the panel. `POST /run/start` (and `/run/resume-from`)
  return `202`, the panel counts down
  (default 5 s, setting `startDelay`) and panel **Reset** cancels it. When the owner switches
  *Settings → Network & API → Allow remote START* off (only possible on the panel, not over
  the API), they return `403 {"error":"remote_start_disabled"}`. Resuming from
  HOLD is a start too. Everything else (load, home, jog, probe, offsets) is allowed
  with the lease.
- **LAN tool, simple auth.** HTTP on the controller's own address
  (`192.168.1.200`, [network.md](network.md)), port `8080`. A bearer token set in
  Settings → Network & API; the API can be switched off there. No cloud, no accounts.

## Transport

| Channel | Use |
|---|---|
| `HTTP/1.1` + JSON, base `/api/v1` | Commands and queries |
| `WebSocket /api/v1/events` | Push: state, position (10–20 Hz while moving), line, alarms, I/O changes, prompts, log |
| `GET /api/v1/openapi.json` | Machine-readable spec of everything below |

Errors: HTTP 4xx with `{"error":"limit_active","detail":"X+ limit tripped","unblock":"jog X- or select another axis"}`.
Long actions (home, probe, run) return `202 {"job":"<id>"}` at once; progress and the
result arrive as events and via `GET /jobs/<id>`.

## Endpoints

### Session
| Method | Path | Body / query | Does |
|---|---|---|---|
| GET | `/status` | | Full snapshot: state, mode, position (machine + work), homed, drives, E-stop, limits, overrides, spindle, coolant, WCS, tool, program, run progress, lease holder |
| POST | `/lease` | `{"take":true}` | Take the control lease (or `false` to release) |
| GET | `/clients` | | Connected clients and roles |

### Machine power & stop
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/machine/drives` | `{"on":true}` | Enable / disable the drives. **No dedicated FPGA drive-enable output has been confirmed on this machine** (the real saved config maps `OUT1`=spindle/M3, `OUT2`=coolant flood/M8, `OUT3`=aux/M10 — see [machine-settings.md](machine-settings.md); drive power may simply be hard-wired, gated only by the drives' own alarm/E-stop lines). Until one is found, this is a software-only interlock, not a real output toggle. |
| POST | `/machine/stop` | | Stop: program aborted, spindle off (any client). With `motion.graceful_stop` (default on) motion brakes along the path at the axes' max deceleration (no lost steps, overshoot ≈ v²/2a); off = halts after the burst in flight |
| POST | `/machine/reset` | | Clear a cleared-condition alarm, return to Idle |
| GET | `/machine/estop` | | E-stop state (pendant `0x3D` bit 9 and external inputs) |

### Homing
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/home` | `{"axes":"all"}` or `{"axes":["Z","X"]}` | Home in configured order (or the given order) |
| GET | `/home` | | Homed flag per axis, last home time |
| POST | `/home/set` | `{"axis":"A","value":0}` | Mark an axis homed at a value (no switch, e.g. rotary) |

### Motion
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/jog` | `{"axis":"X","dir":-1,"step":0.1}` or `{"axis":"Y","dir":1,"continuous":true,"feed":1500}` | Step or continuous jog |
| POST | `/jog/stop` | `{"axis":"Y"}` (optional) | Stop a continuous jog |
| POST | `/move` | `{"mode":"abs","wcs":"G54","x":10,"y":20,"feed":800}` or `{"rapid":true,...}` / `{"mode":"rel",...}` | Coordinated move |
| POST | `/move/safe-z` | | Retract Z to the configured safe height |
| POST | `/move/goto` | `{"target":"wcs-zero"}` · `"park"` · `"tool-change"` | Named positions, Z first |
| POST | `/move/macro` | `{"macro":"safez"}` · `"workzero"` · `"park"` · `"toolchange"` · `"warmup"` | **Implemented (app):** the panel's Macros page. Park / tool change are machine positions (Settings › Macros) and need a homed machine; warm-up runs the spindle for `macro.warm_s` at `macro.warm_rpm`; Reset stops it |
| POST | `/mdi` | `{"line":"G0 X10 Y10"}` | Execute one G-code line (MDI) |

### Probing
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/probe` | `{"axis":"Z","dir":-1,"feed":100,"maxTravel":30,"retract":2}` | Single probe move; returns trigger position |
| POST | `/probe/z-plate` | `{"plate":15.0,"wcs":"G54"}` | Touch off Z on a plate, set WCS Z = plate thickness |
| POST | `/probe/tool-length` | `{"tool":3}` | Measure tool length on the fixed sensor, update tool table |
| POST | `/probe/edge` | `{"axis":"X","dir":1,"toolDiameter":6,"setWcs":true}` | Find an edge |
| POST | `/probe/corner` | `{"corner":"front-left","setWcs":true}` | Find an outside corner (X and Y) |
| POST | `/probe/center` | `{"type":"bore","approxDiameter":30}` | Find a bore or boss centre |
| GET | `/probe/last` | | Last result |

### Programs
| Method | Path | Body / query | Does |
|---|---|---|---|
| GET | `/programs?src=local\|usb\|share` | | List files with size, date |
| POST | `/programs?src=local` | multipart `.nc` file | Upload (from CAM PC) |
| DELETE | `/programs/<name>?src=` | | Delete |
| POST | `/program/load` | `{"src":"local","name":"part.nc"}` | Load and parse; returns errors, line count, bounding box, estimate |
| GET | `/program` | | Loaded program info |
| GET | `/program/lines?from=800&to=860` | | Text of a line range |
| GET | `/program/toolpath?maxPoints=5000` | | Decimated polyline (rapid/feed flagged) for remote viewers |
| GET | `/program/state?line=846` | | Reconstructed modal state + start position at a line |
| GET | `/program/sections` | | Section boundaries (tool changes, G0 retracts, comments) |

### Running
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/run/start` | `{"mode":"run"}` · `"tryCut"` · `"simulate"`; optional `"safeZ":5` for TryCut | Start the loaded program: 202 + panel countdown (Reset cancels); **403 if remote START was switched off on the panel** |
| POST | `/run/pause` | | Feed hold |
| POST | `/run/resume` | | Continue from hold (a start: same remote-START rule) |
| POST | `/run/stop` | | Stop program (motion halts, spindle off) |
| POST | `/run/resume-from` | `{"line":846,"confirmPosition":true,"approach":"safe-z"}` | Restore state for `line`, move there at safe Z, continue. `confirmPosition` is the API form of the panel's one confirmation |
| POST | `/run/prompt` | `{"answer":"continue"}` | Answer an M0/M1/M6 prompt |
| GET | `/run/log?run=<id>` | | Work-done log (line, state, position per block) |
| PUT | `/overrides` | `{"feed":110,"spindle":95,"rapid":50}` | Overrides in %, any subset. Feed and spindle 0–300 % (bars on the panel show up to 200 %); rapid follows feed, max 100 % |

### Spindle & coolant
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/spindle` | `{"on":true,"dir":"cw","rpm":12000}` | Spindle (OUT1 enable + PWM6 speed, [fpga-outputs.md](fpga-outputs.md)) |
| POST | `/coolant` | `{"flood":true,"mist":false}` | Coolant (OUT2 / mapped output) |

### Offsets & tools
| Method | Path | Body | Does |
|---|---|---|---|
| GET / PUT | `/offsets/<G54..G59>` | `{"x":..,"y":..,"z":..,"a":..,"job":true}` | Read / write a WCS; `job` = part of the multi-part job |
| POST | `/offsets/<wcs>/set-current` | `{"axes":["X","Y"]}` | Zero those axes at the current position |
| POST | `/offsets/<wcs>/touch-off` | `{"axis":"X","dir":"+","diameter":12}` | Panel Shift+axis: axis = −dir·Ø/2 at the current position (Ø omitted/0 → 0) |
| POST | `/offsets/active` | `{"wcs":"G55"}` | Select the active WCS |
| GET / PUT | `/tools`, `/tools/<n>` | `{"length":..,"diameter":..,"note":".."}` | Tool table |
| POST | `/tools/active` | `{"tool":3}` | Set the loaded tool (manual change) |

### I/O
| Method | Path | Body | Does |
|---|---|---|---|
| GET | `/io` | | Inputs IN01–13 (state + assigned function), outputs OUT1–3, pendant inputs, limits per axis/direction |
| PUT | `/io/outputs/<1..3>` | `{"on":true}` | Set an output |

### Settings
| Method | Path | Body | Does |
|---|---|---|---|
| GET / PUT | `/settings/<section>` | section JSON | `axes` (steps/mm, max feed, accel, travel, DIR/pulse polarity), `limits` (input mapping, shared inputs, policy), `homing` (order, direction, speeds), `spindle`, `io`, `pendant`, `display` (`posDecimals`, `toolDecimals` 1-3), `network` (IP, API on/off, token; `remoteStart` and `startDelay` are **read-only over the API**) |

### Panel messages (agents)
| Method | Path | Body | Does |
|---|---|---|---|
| POST | `/panel/instructions` | `{"title":"Shelf bracket","steps":[{"text":"Clamp 120×80 stock front-left","check":"operator"},{"text":"Touch off X/Y on G54","check":"touched:X,Y"},...]}` | Show an instruction card on the Run screen. `check` lets the panel tick a step itself (`homed`, `loaded:<file>`, `touched:<axes>`, `operator` = Select A) |
| DELETE | `/panel/instructions` | | Remove the card |
| POST | `/panel/toast` | `{"text":"Tool change next: T4"}` | Short message on the panel |

### Diagnostics (service mode only)
| Method | Path | Body | Does |
|---|---|---|---|
| GET | `/diag/fpga/<addr>` | | Read an FPGA register |
| PUT | `/diag/fpga/<addr>` | `{"value":..}` | Write an FPGA register (only with Settings → Service mode on) |
| POST | `/diag/trace` | `{"on":true}` | Start/stop the `/dev/motion-trace` capture |
| GET | `/diag/trace` | | Download the capture |

## Events (`/api/v1/events`)

```json
{"type":"state","state":"run","mode":"auto","armed":"run"}
{"type":"pos","t":1792310000.12,"mach":{"x":464.207,"y":274.491,"z":-91.201,"a":-1},"work":{"x":190.697,"y":136.788,"z":5,"a":-1}}
{"type":"line","line":842,"of":3110,"pct":27.1,"elapsed":1122,"remaining":2950}
{"type":"alarm","code":"limit","axis":"X","dir":1,"blocked":["X+"],"free":["X-","Y","Z","A"]}
{"type":"alarm-clear","code":"limit","axis":"X"}
{"type":"prompt","kind":"M6","tool":4,"text":"Fit T4 (6 mm flat)"}
{"type":"io","in":{"IN03":true},"out":{"OUT1":true}}
{"type":"lease","holder":"panel"}
```

## Typical job sequence (automation)

```
POST /lease                    {"take":true}
POST /machine/drives           {"on":true}
POST /home                     {"axes":"all"}            -> wait job done
POST /programs?src=local       (upload part.nc)
POST /program/load             {"src":"local","name":"part.nc"}
POST /move/goto                {"target":"wcs-zero"}
POST /probe/z-plate            {"plate":15.0,"wcs":"G54"} -> wait job done
POST /run/start                {"mode":"tryCut","safeZ":5}  (optional check)
POST /run/start                {"mode":"run"}
WS   /events                   watch "line" / "alarm" / "prompt"
POST /run/prompt               {"answer":"continue"}       (on M6)
POST /machine/drives           {"on":false}                (when done)
```

## Mapping to the hardware we know

| API area | Implemented through |
|---|---|
| Jog / move / home | FPGA move sequence `0x00` axis, `0x0A/0x0B` steps, `0x03` mode, `0x04` go+DIR ([fpga-outputs.md](fpga-outputs.md)); **open: the move-done acknowledge** |
| Spindle | OUT1 (`0x3E` bit 0, M3) + PWM6 duty via ioctl `0x4627` |
| Coolant | OUT2 flood (`0x3E` bit 1, M8); OUT3 (bit 2, M10) is an unused aux output on this machine, not drive enable |
| Limits, home, probe | One shared input per axis on this machine (X=IN02, Y=IN03, Z=IN04 — same port serves +limit/−limit/home), external E-stop on IN01, probe port unassigned ([machine-settings.md](machine-settings.md)); the FPGA register for these inputs isn't identified yet, same diff-sweep method as `0x3D` would find it |
| Pendant | `0x3D` (axis, multiplier, E-stop), `0x0E` (wheel) ([mpg-pendant-io.md](mpg-pendant-io.md)) |
| Keypad / LEDs | TM1638 direct scan / LED writes ([ui-controls-map.md](ui-controls-map.md)); `MOTION_IOC_KEY` still broken |
| Trace | `/dev/motion-trace` (our driver) |
