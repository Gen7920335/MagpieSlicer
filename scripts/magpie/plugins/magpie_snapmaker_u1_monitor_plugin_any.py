# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Snapmaker U1 Monitor"
# description = "A device page for the Snapmaker U1: status, temperatures, camera, files, console and print control over Moonraker."
# author = "Magpie"
# version = "1.0.0"
# ///
"""Snapmaker U1 Monitor (Magpie line 1's U1 device panel, rebuilt as an Orca pages plugin).

The page talks to the printer's Moonraker API. The rules that keep the printer safe are
plain functions below the client, the same as line 1's SnapmakerMonitorUtils:

- manual motion and extrusion only while no print runs (standby, complete, cancelled, error);
- pause, resume, cancel and skip-object only while a print runs or is paused;
- emergency stop is always available once a server is set, on its own request;
- heater targets, jog distances and object names are bounded or checked before a command is built.

  page   --orca.postMessage({command: ...})-->  page.on_message()
  plugin --post_message({command: 'status', ...})-->  page (orca.onMessage)
"""
import json
import math
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# --- safety rules -----------------------------------------------------------------------------

STOPPED_STATES = ("standby", "complete", "cancelled", "error")
# Highest heater target accepted, degC; the U1's hotends and bed stay well below it.
MAX_HEATER_TARGET_C = 400.0
# Largest single jog or manual extrusion, mm.
MAX_JOG_MM = 100.0
OBJECT_NAME = re.compile(r"^[A-Za-z0-9_.\-]+$")


def manual_motion_allowed(connected, command_in_flight, print_state):
    return bool(connected) and not command_in_flight and print_state in STOPPED_STATES


def emergency_stop_allowed(has_server, emergency_in_flight):
    return bool(has_server) and not emergency_in_flight


def control_availability(connected, command_in_flight, has_server, print_state, exclude_object_supported, object_selected):
    controllable = bool(connected) and not command_in_flight and print_state in ("printing", "paused")
    return {
        "pause_resume": controllable,
        "cancel": controllable,
        "refresh": bool(has_server) and not command_in_flight,
        "skip_object": controllable and bool(exclude_object_supported) and bool(object_selected),
        "resume_mode": print_state == "paused",
    }


def is_valid_object_name(name):
    return isinstance(name, str) and OBJECT_NAME.match(name) is not None


def is_public_macro(name):
    if not is_valid_object_name(name) or name.startswith("_") or name in ("CANCEL_PRINT", "PAUSE", "RESUME"):
        return False
    return not (len(name) > 1 and name[0] == "T" and name[1:].isdigit())


def _finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def jog_script(axis, distance):
    axis = str(axis).upper()
    if axis not in ("X", "Y", "Z"):
        raise ValueError("invalid jog axis")
    if not _finite(distance) or distance == 0 or abs(distance) > MAX_JOG_MM:
        raise ValueError("invalid jog distance")
    return f"G91\nG0 {axis}{distance:.3f} F{600 if axis == 'Z' else 6000}\nG90"


def heater_script(heater, target):
    if not is_valid_object_name(heater) or not _finite(target) or target < 0 or target > MAX_HEATER_TARGET_C:
        raise ValueError("invalid heater command")
    return f"SET_HEATER_TEMPERATURE HEATER={heater} TARGET={target:.1f}"


def extrude_script(distance, speed_mm_s):
    if (not _finite(distance) or distance == 0 or abs(distance) > MAX_JOG_MM or
            not _finite(speed_mm_s) or speed_mm_s < 0.1 or speed_mm_s > 100):
        raise ValueError("invalid manual extrusion command")
    return (f"SAVE_GCODE_STATE NAME=MAGPIE_MANUAL_EXTRUDE\nM83\nG1 E{distance:.3f} F{speed_mm_s * 60:.1f}\n"
            "RESTORE_GCODE_STATE NAME=MAGPIE_MANUAL_EXTRUDE")


def normalize_base_url(value):
    value = (value or "").strip().rstrip("/")
    parts = urllib.parse.urlsplit(value)
    if parts.scheme not in ("http", "https") or not parts.netloc:
        raise ValueError("the printer address must start with http:// or https:// and name a host")
    return f"{parts.scheme}://{parts.netloc}"


# --- Moonraker client -------------------------------------------------------------------------

HEATERS = ("extruder", "extruder1", "extruder2", "extruder3", "heater_bed")
STATUS_OBJECTS = ("print_stats", "virtual_sdcard", "display_status", "exclude_object") + HEATERS


class MoonrakerError(Exception):
    pass


class MoonrakerClient:
    def __init__(self, base_url, api_key="", timeout_s=5.0):
        self.base_url = normalize_base_url(base_url)
        self.api_key = api_key
        self.timeout_s = timeout_s

    def _request(self, method, path, params=None):
        url = self.base_url + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        request = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
        if self.api_key:
            request.add_header("X-Api-Key", self.api_key)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
                body = response.read()
        except urllib.error.HTTPError as e:
            raise MoonrakerError(f"HTTP {e.code} from {path}") from None
        except (urllib.error.URLError, OSError) as e:
            raise MoonrakerError(f"cannot reach {self.base_url}: {getattr(e, 'reason', e)}") from None
        try:
            return json.loads(body.decode("utf-8")).get("result")
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            raise MoonrakerError(f"unexpected reply from {path}") from None

    def status(self):
        result = self._request("GET", "/printer/objects/query", {name: "" for name in STATUS_OBJECTS})
        return summarize_status((result or {}).get("status", {}))

    def gcode(self, script):
        return self._request("POST", "/printer/gcode/script", {"script": script})

    def pause(self):
        return self._request("POST", "/printer/print/pause")

    def resume(self):
        return self._request("POST", "/printer/print/resume")

    def cancel(self):
        return self._request("POST", "/printer/print/cancel")

    def emergency_stop(self):
        return self._request("POST", "/printer/emergency_stop")

    def files(self):
        result = self._request("GET", "/server/files/list", {"root": "gcodes"}) or []
        files = [{"path": f.get("path", ""), "size": f.get("size", 0), "modified": f.get("modified", 0)} for f in result]
        return sorted(files, key=lambda f: f["modified"], reverse=True)

    def start_print(self, filename):
        return self._request("POST", "/printer/print/start", {"filename": filename})

    def console(self, count=50):
        result = self._request("GET", "/server/gcode_store", {"count": count}) or {}
        return [{"message": e.get("message", ""), "time": e.get("time", 0), "type": e.get("type", "")}
                for e in result.get("gcode_store", [])]

    def snapshot_url(self):
        try:
            cams = (self._request("GET", "/server/webcams/list") or {}).get("webcams", [])
        except MoonrakerError:
            cams = []
        for cam in cams:
            url = cam.get("snapshot_url", "")
            if url:
                return url if "://" in url else self.base_url + url
        return self.base_url + "/webcam/?action=snapshot"


def summarize_status(status):
    stats = status.get("print_stats", {}) or {}
    sdcard = status.get("virtual_sdcard", {}) or {}
    display = status.get("display_status", {}) or {}
    exclude = status.get("exclude_object", {}) or {}
    info = stats.get("info", {}) or {}
    heaters = {}
    for name in HEATERS:
        h = status.get(name)
        if isinstance(h, dict) and "temperature" in h:
            heaters[name] = {"temperature": h.get("temperature", 0.0), "target": h.get("target", 0.0)}
    return {
        "state": stats.get("state", "standby"),
        "filename": stats.get("filename", ""),
        "message": stats.get("message", "") or display.get("message", "") or "",
        "progress": float(display.get("progress", sdcard.get("progress", 0.0)) or 0.0),
        "print_duration": float(stats.get("print_duration", 0.0) or 0.0),
        "layer": info.get("current_layer"),
        "layers": info.get("total_layer"),
        "heaters": heaters,
        "objects": [o.get("name", "") for o in exclude.get("objects", []) if is_valid_object_name(o.get("name", ""))],
        "excluded": list(exclude.get("excluded_objects", [])),
        "exclude_supported": "exclude_object" in status,
    }


# --- page ---------------------------------------------------------------------------------------

PAGE = """
<style>
  body { margin: 0; padding: 12px; font-size: 13px; }
  h3 { margin: 12px 0 6px; font-size: 14px; }
  .row { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin-bottom: 6px; }
  .muted { color: var(--orca-muted); font-size: 12px; }
  .danger { background: #c0392b; color: #fff; border-color: #c0392b; }
  table { border-collapse: collapse; width: 100%; }
  td, th { padding: 2px 6px; text-align: left; }
  td.num { text-align: right; font-variant-numeric: tabular-nums; }
  #camera { max-width: 100%; max-height: 320px; background: #000; }
  #console { height: 140px; overflow: auto; font-family: monospace; font-size: 12px; white-space: pre-wrap;
             border: 1px solid var(--orca-border); padding: 4px; }
  .grid { display: grid; grid-template-columns: minmax(280px, 1fr) minmax(280px, 1fr); gap: 16px; }
</style>

<div class="row">
  <input id="url" size="32" placeholder="http://printer-address">
  <button type="button" id="connect">Connect</button>
  <button type="button" id="estop" class="danger" disabled>Emergency stop</button>
  <span id="conn" class="muted">Not connected</span>
</div>

<div class="grid">
<div>
  <h3>Print</h3>
  <div id="state">-</div>
  <div class="muted" id="job"></div>
  <progress id="progress" max="1" value="0" style="width:100%"></progress>
  <div class="row">
    <button type="button" id="pause" disabled>Pause</button>
    <button type="button" id="cancel" disabled>Cancel</button>
    <select id="objects"></select>
    <button type="button" id="skip" disabled>Skip object</button>
  </div>

  <h3>Temperatures</h3>
  <table><thead><tr><th>Heater</th><th>Now</th><th>Target</th><th>Set</th></tr></thead><tbody id="heaters"></tbody></table>

  <h3>Move</h3>
  <div class="row" id="jog">
    <button type="button" data-axis="X" data-d="-10">X-10</button><button type="button" data-axis="X" data-d="10">X+10</button>
    <button type="button" data-axis="Y" data-d="-10">Y-10</button><button type="button" data-axis="Y" data-d="10">Y+10</button>
    <button type="button" data-axis="Z" data-d="-1">Z-1</button><button type="button" data-axis="Z" data-d="1">Z+1</button>
    <button type="button" id="home">Home</button>
  </div>
  <div class="muted">Moving is possible only while no print runs.</div>
</div>
<div>
  <h3>Camera</h3>
  <img id="camera" alt="">
  <h3>Files</h3>
  <select id="files" size="6" style="width:100%"></select>
  <div class="row"><button type="button" id="print" disabled>Print selected</button><button type="button" id="reload">Reload</button></div>
  <h3>Console</h3>
  <div id="console"></div>
  <div class="row"><input id="cmd" size="30" placeholder="G-code"><button type="button" id="send" disabled>Send</button></div>
</div>
</div>
<p id="error" class="muted"></p>

<script>
(function () {
  var $ = function (id) { return document.getElementById(id); };
  var state = { avail: {}, motion: false, connected: false };
  function post(m) { orca.postMessage(m); }
  function text(v) { var s = document.createElement("span"); s.textContent = v; return s.innerHTML; }

  function render(m) {
    state.connected = m.connected; state.avail = m.availability || {}; state.motion = m.motion_allowed;
    $("conn").textContent = m.connected ? "Connected to " + m.url : (m.url ? "Cannot reach " + m.url : "Not connected");
    $("estop").disabled = !m.estop_allowed;
    var s = m.status || {};
    $("state").textContent = s.state ? s.state + (s.message ? " - " + s.message : "") : "-";
    $("job").textContent = s.filename ? s.filename + (s.layers ? "  layer " + (s.layer || 0) + "/" + s.layers : "") : "";
    $("progress").value = s.progress || 0;
    $("pause").disabled = !state.avail.pause_resume;
    $("pause").textContent = state.avail.resume_mode ? "Resume" : "Pause";
    $("cancel").disabled = !state.avail.cancel;
    var sel = $("objects"), keep = sel.value;
    sel.innerHTML = (s.objects || []).filter(function (o) { return (s.excluded || []).indexOf(o) < 0; })
      .map(function (o) { return "<option>" + text(o) + "</option>"; }).join("");
    if (keep) sel.value = keep;
    $("skip").disabled = !(state.avail.pause_resume && s.exclude_supported && sel.value);
    var rows = "";
    Object.keys(s.heaters || {}).forEach(function (h) {
      var v = s.heaters[h];
      rows += "<tr><td>" + text(h) + "</td><td class='num'>" + v.temperature.toFixed(1) + "</td><td class='num'>" +
              v.target.toFixed(0) + "</td><td><input size='4' data-heater='" + text(h) + "'><button type='button' data-set='" +
              text(h) + "'>Set</button></td></tr>";
    });
    if (document.activeElement && document.activeElement.dataset && document.activeElement.dataset.heater) {} else $("heaters").innerHTML = rows;
    Array.prototype.forEach.call(document.querySelectorAll("#jog button"), function (b) { b.disabled = !state.motion; });
    $("send").disabled = !m.connected;
    $("print").disabled = !state.motion;
    if (m.camera) $("camera").src = m.camera + (m.camera.indexOf("?") < 0 ? "?" : "&") + "t=" + Date.now();
    $("error").textContent = m.error || "";
  }

  orca.onMessage(function (m) {
    if (!m) return;
    if (m.command === "status") render(m);
    else if (m.command === "files")
      $("files").innerHTML = m.files.map(function (f) { return "<option value='" + text(f.path) + "'>" + text(f.path) + "</option>"; }).join("");
    else if (m.command === "console")
      $("console").textContent = m.lines.map(function (l) { return l.message; }).join("\\n");
    else if (m.command === "url") $("url").value = m.url;
    else if (m.command === "error") $("error").textContent = m.error;
  });

  $("connect").onclick = function () { post({ command: "connect", url: $("url").value }); };
  $("estop").onclick = function () { post({ command: "emergency_stop" }); };
  $("pause").onclick = function () { post({ command: state.avail.resume_mode ? "resume" : "pause" }); };
  $("cancel").onclick = function () { if (confirm("Cancel the print?")) post({ command: "cancel" }); };
  $("skip").onclick = function () { if (confirm("Skip " + $("objects").value + "?")) post({ command: "skip_object", name: $("objects").value }); };
  $("home").onclick = function () { post({ command: "home" }); };
  $("reload").onclick = function () { post({ command: "files" }); };
  $("print").onclick = function () { if ($("files").value) post({ command: "start_print", filename: $("files").value }); };
  $("send").onclick = function () { post({ command: "gcode", script: $("cmd").value }); $("cmd").value = ""; };
  $("jog").addEventListener("click", function (e) {
    var b = e.target; if (b.dataset && b.dataset.axis) post({ command: "jog", axis: b.dataset.axis, distance: parseFloat(b.dataset.d) });
  });
  $("heaters").addEventListener("click", function (e) {
    var h = e.target.dataset && e.target.dataset.set; if (!h) return;
    var input = document.querySelector("input[data-heater='" + h + "']");
    post({ command: "heater", heater: h, target: parseFloat(input.value) });
  });
  post({ command: "ready" });
})();
</script>
"""


class U1Monitor:
    """Page-independent controller: holds the connection, polls status and turns page commands
    into Moonraker requests after the safety checks. `send` posts a dict to the page."""

    POLL_INTERVAL_S = 2.0

    def __init__(self, send, client_factory=MoonrakerClient):
        self.send = send
        self.client_factory = client_factory
        self.client = None
        self.url = ""
        self.connected = False
        self.status = {}
        self.camera = ""
        self.command_in_flight = False
        self.emergency_in_flight = False
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.thread = None

    def snapshot(self, error=""):
        state = self.status.get("state", "")
        return {
            "command": "status",
            "url": self.url,
            "connected": self.connected,
            "status": self.status,
            "camera": self.camera if self.connected else "",
            "motion_allowed": manual_motion_allowed(self.connected, self.command_in_flight, state),
            "estop_allowed": emergency_stop_allowed(self.client is not None, self.emergency_in_flight),
            "availability": control_availability(self.connected, self.command_in_flight, self.client is not None, state,
                                                 self.status.get("exclude_supported", False), True),
            "error": error,
        }

    def connect(self, url):
        self.client = self.client_factory(url)
        self.url = self.client.base_url
        self.camera = self.client.snapshot_url()
        self.refresh()
        if self.thread is None:
            self.thread = threading.Thread(target=self._poll, daemon=True)
            self.thread.start()

    def refresh(self):
        if self.client is None:
            return
        try:
            self.status = self.client.status()
            self.connected = True
            self.send(self.snapshot())
        except MoonrakerError as e:
            self.connected = False
            self.send(self.snapshot(str(e)))

    def _poll(self):
        while not self.stop_event.wait(self.POLL_INTERVAL_S):
            self.refresh()

    def stop(self):
        self.stop_event.set()

    def _run(self, action):
        with self.lock:
            self.command_in_flight = True
        try:
            action()
            error = ""
        except (MoonrakerError, ValueError) as e:
            error = str(e)
        finally:
            with self.lock:
                self.command_in_flight = False
        self.refresh()
        if error:
            self.send({"command": "error", "error": error})

    def handle(self, message):
        cmd = message.get("command")
        if cmd == "connect":
            try:
                self.connect(message.get("url", ""))
            except ValueError as e:
                self.send({"command": "error", "error": str(e)})
            return
        if self.client is None:
            if cmd != "ready":
                self.send({"command": "error", "error": "Connect to the printer first."})
            return
        state = self.status.get("state", "")
        avail = control_availability(self.connected, self.command_in_flight, True, state,
                                     self.status.get("exclude_supported", False), True)
        motion = manual_motion_allowed(self.connected, self.command_in_flight, state)
        if cmd == "emergency_stop":
            if not emergency_stop_allowed(True, self.emergency_in_flight):
                return
            self.emergency_in_flight = True
            try:
                self.client.emergency_stop()
            except MoonrakerError as e:
                self.send({"command": "error", "error": str(e)})
            finally:
                self.emergency_in_flight = False
            self.refresh()
        elif cmd in ("pause", "resume", "cancel"):
            if avail["pause_resume"]:
                self._run(getattr(self.client, cmd))
        elif cmd == "skip_object":
            name = message.get("name", "")
            if avail["pause_resume"] and self.status.get("exclude_supported") and is_valid_object_name(name):
                self._run(lambda: self.client.gcode(f"EXCLUDE_OBJECT NAME={name}"))
        elif cmd == "heater":
            target = message.get("target")
            self._run(lambda: self.client.gcode(heater_script(message.get("heater", ""), target)))
        elif cmd == "jog":
            if motion:
                self._run(lambda: self.client.gcode(jog_script(message.get("axis", ""), message.get("distance"))))
        elif cmd == "home":
            if motion:
                self._run(lambda: self.client.gcode("G28"))
        elif cmd == "start_print":
            if motion and message.get("filename"):
                self._run(lambda: self.client.start_print(message["filename"]))
        elif cmd == "gcode":
            script = (message.get("script") or "").strip()
            if script:
                self._run(lambda: self.client.gcode(script))
                try:
                    self.send({"command": "console", "lines": self.client.console()})
                except MoonrakerError:
                    pass
        elif cmd == "files":
            try:
                self.send({"command": "files", "files": self.client.files()})
            except MoonrakerError as e:
                self.send({"command": "error", "error": str(e)})


# --- Orca plugin --------------------------------------------------------------------------------

try:
    import orca
except ImportError:  # imported by the tests without the app
    orca = None

if orca is not None:
    class U1MonitorPage(orca.pages.PagesPluginCapabilityBase):
        def __init__(self):
            super().__init__()
            self.monitor = U1Monitor(self.post_message)

        def get_name(self):
            return "Snapmaker U1"

        def get_ui(self):
            return PAGE

        def on_message(self, message):
            try:
                data = json.loads(message) if isinstance(message, str) else message
            except json.JSONDecodeError:
                return
            if not isinstance(data, dict):
                return
            if data.get("command") == "ready":
                host = ""
                try:
                    host = orca.host.preset_bundle().full_config_value("print_host")
                except Exception:
                    pass
                if host:
                    self.post_message({"command": "url", "url": host if "://" in host else "http://" + host})
            # Commands block on the network; keep the page responsive.
            threading.Thread(target=self.monitor.handle, args=(data,), daemon=True).start()

        def on_unload(self):
            self.monitor.stop()

    @orca.plugin
    class SnapmakerU1MonitorPlugin(orca.base):
        def register_capabilities(self):
            orca.register_capability(U1MonitorPage)
