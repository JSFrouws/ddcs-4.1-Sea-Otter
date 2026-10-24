"""API smoke test: exercises every endpoint once and checks the answers.

    python tools/api_smoke.py http://localhost:8099        # the simulator (seaotter --sim)
    python tools/api_smoke.py http://192.168.1.200:8080    # a controller (moves only a few mm)

Leaves the machine as it found it: work offsets, tools, loaded tool, settings and screen are
read first and put back at the end. On a real controller the moves are 1-2 mm jogs, a spindle
on/off without a spindle connected, and Safe Z; no program is run."""
import json, sys, time, urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8099").rstrip("/") + "/api/v1/"


def call(path, body=None, method=None, raw=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, data=data, method=method or ("POST" if data is not None else "GET"))
    try:
        r = urllib.request.urlopen(req, timeout=15)
        return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read() or b"{}")
        except ValueError: return e.code, {}


def idle(t=30):
    """until the steps stop changing (jogs run while the state stays IDLE) and the FPGA agrees"""
    t0, last, same = time.time(), None, 0
    time.sleep(0.3)
    while time.time() - t0 < t:
        s = call("status")[1]
        same = same + 1 if s["steps"] == last else 0
        last = s["steps"]
        if same >= 3 and s["fpga"] == s["steps"] and s["state"] in ("IDLE", "ESTOP", "OFF", "LIMIT"): return s
        time.sleep(0.2)
    return call("status")[1]


results = []
def check(name, cond, info=""):
    results.append((name, bool(cond)))
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {info}")


def off(c, r):
    """403 not_allowed: that API action is switched off on the panel (Settings > API)"""
    return c == 403 and r.get("error") == "not_allowed"


st = call("status")[1]
print("state", st["state"], "backend", st["backend"])
offs0 = call("offsets")[1]
tools0 = call("tools")[1]
settings0 = call("settings")[1]

c, r = call("offsets"); check("GET offsets", c == 200 and "G54" in r["wcs"] and r["active"].startswith("G5"), r["active"])
c, r = call("tools"); check("GET tools", c == 200 and len(r["tools"]) == 12, f"loaded T{r.get('loaded')}")
c, r = call("programs"); check("GET programs (local)", c == 200 and isinstance(r["files"], list), f"{len(r['files'])} files")
c, r = call("programs?source=usb"); check("GET programs (usb)", c == 200 and r["source"] == "usb", f"{len(r['files'])} files")
c, r = call("program/upload?name=api-smoke.nc", raw=b"(api smoke)\nG21 G90\nT1 M6\nG0 X1 Y1\nT2 M6\nG0 X2\nM30\n")
check("POST program/upload", c == 200 and r["lines"] == 7)
c, r = call("program"); check("GET program", c == 200 and r["loaded"] and r["name"] == "api-smoke.nc" and len(r["tools"]) == 2, str(r.get("tools")))
c, r = call("program/lines?from=3&count=2"); check("GET program/lines", c == 200 and r["lines"] == ["T1 M6", "G0 X1 Y1"], str(r.get("lines")))
c, r = call("program/load", {"name": "api-smoke.nc"}); check("POST program/load", c == 200 and r["ok"])
c, r = call("program/load", {"name": "does-not-exist.nc"}); check("POST program/load (missing file) is refused", c == 404 and call("program")[1]["name"] == "api-smoke.nc")
c, r = call("run/arm", {"mode": "simulate"}); check("POST run/arm", c == 200 and call("status")[1]["armed"] == "simulate")
call("run/arm", {"mode": "run"})
c, r = call("run/rewind", {}); check("POST run/rewind", c == 200)
c, r = call("tools/T12", {"dia": 1.5, "len": 33.3, "note": "api smoke"})
t12 = call("tools")[1]["tools"][11]
check("POST tools/T12", c == 200 and t12["dia"] == 1.5 and t12["note"] == "api smoke")
c, r = call("tools/loaded", {"tool": 12}); check("POST tools/loaded", c == 200 and call("tools")[1]["loaded"] == 12)
c, r = call("panel/screen", {"screen": "files"}); check("POST panel/screen", c == 200 and call("status")[1]["screen"] == 2)
call("panel/screen", {"screen": "run"})
c, r = call("panel/toast", {"text": "API smoke test running"}); check("POST panel/toast", c == 200)

perm = {k: settings0.get(k, 1) for k in ("api.jog", "api.moves", "api.spindle", "api.coolant")}
for path, body, key in [("jog", {"axis": "X", "dist": 1}, "api.jog"), ("move/gotozero", {"zOnly": True}, "api.moves"),
                        ("machine/spindle", {"on": True}, "api.spindle"), ("machine/coolant", {"on": True}, "api.coolant")]:
    if not perm[key]:
        c, r = call(path, body); check(f"{path} refused while {key} is off", off(c, r), r.get("detail", ""))
moving_ok = st["state"] == "IDLE" and all(perm.values())
if moving_ok:
    c, r = call("machine/spindle", {"on": True, "rpm": 3000}); s = call("status")[1]
    check("POST machine/spindle on", c == 200 and s["spindle"]["on"] and s["spindle"]["rpm"] == 3000)
    c, r = call("machine/spindle", {"on": False}); check("POST machine/spindle off", c == 200 and not call("status")[1]["spindle"]["on"])
    c, r = call("machine/coolant", {"on": True}); s = call("status")[1]
    check("POST machine/coolant on", c == 200 and s["coolant"] & 1)
    call("machine/coolant", {"on": False})
    s0 = idle()
    c, r = call("jog", {"axis": "X", "dist": 1}); s1 = idle()
    check("POST jog", c == 200 and abs(s1["machine"]["x"] - s0["machine"]["x"] - 1) < 1e-3)
    call("jog", {"axis": "X", "dist": -1}); idle()
    c, r = call("move/gotozero", {"zOnly": True}); s2 = idle()
    check("POST move/gotozero zOnly (Safe Z)", c == 200 and abs(s2["machine"]["x"] - s0["machine"]["x"]) < 1e-3, f"Z work {s2['work']['z']}")
else:
    print(f"(state {st['state']}: moves, spindle and coolant skipped)")
    if perm["api.jog"]:
        c, r = call("jog", {"axis": "X", "dist": 1}); check(f"POST jog refused in {st['state']}", c == 409, r.get("detail", ""))

c, r = call("settings/defaults", {}) if False else (200, {})   # not run: it would reset this machine's settings

# put everything back
for name, w in offs0["wcs"].items():
    call(f"offsets/{name}", w)
call("offsets/active", {"wcs": offs0["active"]})
t = tools0["tools"][11]; call("tools/T12", {"dia": t["dia"], "len": t["len"], "note": t["note"]})
call("tools/loaded", {"tool": tools0["loaded"]})
call("settings", {k: settings0[k] for k in ("spindle.default",)}, "PUT")
print("\n%d / %d passed" % (sum(v for _, v in results), len(results)))
sys.exit(0 if all(v for _, v in results) else 1)
