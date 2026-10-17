"""Tests for the Snapmaker U1 Monitor plugin against a fake Moonraker server.

    python -m unittest scripts/magpie/plugins/test_snapmaker_u1_monitor.py
"""
import http.server
import importlib.util
import json
import os
import threading
import unittest
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("u1", os.path.join(HERE, "magpie_snapmaker_u1_monitor_plugin_any.py"))
u1 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u1)


class FakeMoonraker(http.server.BaseHTTPRequestHandler):
    """Answers the endpoints the plugin uses and records every request."""
    requests = []
    state = "printing"

    def log_message(self, *args):
        pass

    def _reply(self, result, code=200):
        body = json.dumps({"result": result}).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle(self, method):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True))
        FakeMoonraker.requests.append((method, url.path, query))
        if url.path == "/printer/objects/query":
            self._reply({"status": {
                "print_stats": {"state": FakeMoonraker.state, "filename": "cube.gcode", "print_duration": 12.5,
                                "info": {"current_layer": 3, "total_layer": 50}},
                "display_status": {"progress": 0.25, "message": ""},
                "virtual_sdcard": {"progress": 0.24},
                "extruder": {"temperature": 219.6, "target": 220.0},
                "extruder1": {"temperature": 25.0, "target": 0.0},
                "heater_bed": {"temperature": 60.1, "target": 60.0},
                "exclude_object": {"objects": [{"name": "cube_id_0_copy_0"}, {"name": "bad name;G28"}],
                                   "excluded_objects": []},
            }})
        elif url.path == "/server/webcams/list":
            self._reply({"webcams": [{"snapshot_url": "/webcam/snapshot.jpg"}]})
        elif url.path == "/server/files/list":
            self._reply([{"path": "old.gcode", "size": 10, "modified": 1}, {"path": "new.gcode", "size": 20, "modified": 2}])
        elif url.path == "/server/gcode_store":
            self._reply({"gcode_store": [{"message": "ok", "time": 1, "type": "response"}]})
        elif url.path == "/printer/missing":
            self._reply(None, 404)
        else:
            self._reply("ok")

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")


class MonitorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeMoonraker)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        FakeMoonraker.requests.clear()
        FakeMoonraker.state = "printing"
        self.sent = []
        self.monitor = u1.U1Monitor(self.sent.append)
        self.monitor.POLL_INTERVAL_S = 3600  # no background polling inside a test

    def tearDown(self):
        self.monitor.stop()

    def posted(self, path):
        return [r for r in FakeMoonraker.requests if r[0] == "POST" and r[1] == path]

    def test_status_is_summarized_and_unsafe_object_names_are_dropped(self):
        self.monitor.connect(self.url + "/some/path/")
        self.assertEqual(self.monitor.url, self.url)
        s = self.sent[-1]["status"]
        self.assertTrue(self.sent[-1]["connected"])
        self.assertEqual(s["state"], "printing")
        self.assertEqual((s["layer"], s["layers"]), (3, 50))
        self.assertAlmostEqual(s["progress"], 0.25)
        self.assertEqual(s["heaters"]["heater_bed"], {"temperature": 60.1, "target": 60.0})
        self.assertEqual(s["objects"], ["cube_id_0_copy_0"])
        self.assertEqual(self.sent[-1]["camera"], self.url + "/webcam/snapshot.jpg")

    def test_motion_is_refused_while_printing_and_allowed_when_stopped(self):
        self.monitor.connect(self.url)
        self.monitor.handle({"command": "jog", "axis": "X", "distance": 10})
        self.monitor.handle({"command": "home"})
        self.monitor.handle({"command": "start_print", "filename": "new.gcode"})
        self.assertEqual(self.posted("/printer/gcode/script"), [])
        self.assertEqual(self.posted("/printer/print/start"), [])
        FakeMoonraker.state = "complete"
        self.monitor.refresh()
        self.monitor.handle({"command": "jog", "axis": "X", "distance": 10})
        self.assertEqual(self.posted("/printer/gcode/script")[-1][2]["script"], "G91\nG0 X10.000 F6000\nG90")

    def test_print_control_follows_the_print_state(self):
        self.monitor.connect(self.url)
        self.monitor.handle({"command": "pause"})
        self.assertEqual(len(self.posted("/printer/print/pause")), 1)
        FakeMoonraker.state = "standby"
        self.monitor.refresh()
        self.monitor.handle({"command": "cancel"})
        self.assertEqual(self.posted("/printer/print/cancel"), [])

    def test_emergency_stop_works_in_any_state(self):
        self.monitor.connect(self.url)
        for state in ("printing", "standby", "error"):
            FakeMoonraker.state = state
            self.monitor.refresh()
            self.monitor.handle({"command": "emergency_stop"})
        self.assertEqual(len(self.posted("/printer/emergency_stop")), 3)

    def test_skip_object_rejects_names_that_could_inject_commands(self):
        self.monitor.connect(self.url)
        self.monitor.handle({"command": "skip_object", "name": "x\nG28"})
        self.assertEqual(self.posted("/printer/gcode/script"), [])
        self.monitor.handle({"command": "skip_object", "name": "cube_id_0_copy_0"})
        self.assertEqual(self.posted("/printer/gcode/script")[-1][2]["script"], "EXCLUDE_OBJECT NAME=cube_id_0_copy_0")

    def test_heater_targets_are_bounded(self):
        self.monitor.connect(self.url)
        self.monitor.handle({"command": "heater", "heater": "extruder", "target": 999})
        self.assertEqual(self.posted("/printer/gcode/script"), [])
        self.assertIn("invalid heater", self.sent[-1]["error"])
        self.monitor.handle({"command": "heater", "heater": "extruder", "target": 215})
        self.assertEqual(self.posted("/printer/gcode/script")[-1][2]["script"],
                         "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215.0")

    def test_files_newest_first_and_unreachable_printer_reported(self):
        self.monitor.connect(self.url)
        self.monitor.handle({"command": "files"})
        self.assertEqual([f["path"] for f in self.sent[-1]["files"]], ["new.gcode", "old.gcode"])
        dead = u1.U1Monitor(self.sent.append)
        dead.POLL_INTERVAL_S = 3600
        dead.connect("http://127.0.0.1:9")
        self.assertFalse(self.sent[-1]["connected"])
        self.assertIn("cannot reach", self.sent[-1]["error"])
        dead.stop()

    def test_scripts_and_addresses_are_checked(self):
        with self.assertRaises(ValueError):
            u1.jog_script("E", 1)
        with self.assertRaises(ValueError):
            u1.jog_script("Z", 150)
        with self.assertRaises(ValueError):
            u1.extrude_script(5, 500)
        with self.assertRaises(ValueError):
            u1.normalize_base_url("printer.local")
        self.assertFalse(u1.is_public_macro("T1"))
        self.assertFalse(u1.is_public_macro("_HIDDEN"))
        self.assertTrue(u1.is_public_macro("LOAD_FILAMENT"))


if __name__ == "__main__":
    unittest.main()
