"""Independent service and HTTP acceptance tests for the single-Spike tool.

Each test owns a temporary workspace and the HTTP suite uses an ephemeral port.
Numerical geometry checks live in test_spike_calibration_engine; these tests
exercise persistence, data isolation, stale-result prevention and actual routes.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import threading
import types
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import cv2
import numpy as np


def png_file(name="blank.png"):
    ok, binary = cv2.imencode(".png", np.full((60, 80), 127, np.uint8))
    assert ok
    return {"name": name, "content_base64": base64.b64encode(binary).decode("ascii")}


class WorkbenchCase(unittest.TestCase):
    def setUp(self):
        from spike_calibration_tool.service import Workbench
        self.temp = tempfile.TemporaryDirectory(prefix="spike-tool-qa-")
        self.root = Path(self.temp.name)
        self.app = Workbench(self.root)
        self.project = self.app.create_project({"name": "QA Spike 单机"})
        self.pid = self.project["id"]

    def tearDown(self):
        close = getattr(self.app, "close", None)
        if close:
            close()
        self.temp.cleanup()

    def demo(self):
        return self.app.load_demo(self.pid)

    def solved(self):
        self.demo()
        return self.app.solve(self.pid)

    def assert_invalid_results(self, project):
        self.assertIsNone(project["model"])
        self.assertIsNone(project["validation"])
        self.assertIsNone(project["turntable"])


class ServiceAcceptanceTests(WorkbenchCase):
    def test_new_project_has_single_spike_scope_and_no_fabricated_model(self):
        self.assertEqual(self.project["captures"], [])
        self.assert_invalid_results(self.project)
        optics = self.project["config"]["optics"]
        self.assertTrue(optics["shared_lens"])
        self.assertEqual(optics["split_ratio"], "1:1")
        state = self.app.state()
        self.assertEqual(state["app"], "spike-calibration-tool")
        self.assertEqual(state["projects"][0]["id"], self.pid)

    def test_project_survives_reload_and_returned_objects_do_not_mutate_storage(self):
        from spike_calibration_tool.service import Workbench
        self.project["name"] = "mutated outside service"
        self.assertEqual(self.app.get_project(self.pid)["name"], "QA Spike 单机")
        restored = Workbench(self.root)
        try:
            self.assertEqual(restored.get_project(self.pid)["name"], "QA Spike 单机")
        finally:
            close = getattr(restored, "close", None)
            if close:
                close()

    def test_stale_second_window_write_is_rejected_and_refreshes_its_snapshot(self):
        from spike_calibration_tool.service import Workbench
        peer = Workbench(self.root)
        first = self.app.update_config(self.pid, {"config": {"operator": "first window"}})
        with self.assertRaisesRegex(ValueError, "另一窗口"):
            peer.update_config(self.pid, {"config": {"operator": "stale second window"}})
        self.assertEqual(peer.get_project(self.pid), first)
        self.assertEqual(json.loads((self.root / (self.pid + ".json")).read_text(encoding="utf-8")), first)
        second = peer.update_config(self.pid, {"config": {"operator": "refreshed second window"}})
        self.assertEqual(second["revision"], first["revision"] + 1)

    def test_demo_is_explicitly_synthetic_with_disjoint_training_and_test_sequences(self):
        project = self.demo()
        train = {c["sequence_id"] for c in project["captures"] if c["split"] == "train"}
        test = {c["sequence_id"] for c in project["captures"] if c["split"] == "test"}
        self.assertTrue(train)
        self.assertTrue(test)
        self.assertFalse(train & test)
        self.assertEqual({c["evidence"] for c in project["captures"]}, {"synthetic"})
        self.assert_invalid_results(project)

    def test_solve_validate_and_query_are_real_and_validation_keeps_frozen_k(self):
        solved = self.solved()
        k = copy.deepcopy(solved["model"]["K"])
        validated = self.app.validate(self.pid)
        self.assertEqual(validated["model"]["K"], k)
        self.assertIsNotNone(validated["validation"])
        self.assertNotEqual(validated["validation"].get("status"), "passed")
        result = self.app.query(self.pid, {"points": [[k[0][2], k[1][2]]]})
        point = result["points"][0]
        self.assertAlmostEqual(point["azimuth_deg"], 0.0, places=5)
        self.assertAlmostEqual(point["elevation_deg"], 0.0, places=5)
        self.assertAlmostEqual(float(np.linalg.norm(point["ray_camera"])), 1.0, places=6)

    def test_direction_change_clears_observations_and_all_old_results(self):
        project = self.solved()
        config = copy.deepcopy(project["config"])
        config["orientation"]["flip_x"] = True
        changed = self.app.update_config(self.pid, {"config": config})
        self.assertEqual(changed["captures"], [])
        self.assert_invalid_results(changed)
        with self.assertRaises((ValueError, KeyError)):
            self.app.query(self.pid, {"points": [[20, 20]]})

    def test_excluding_capture_invalidates_calibration(self):
        project = self.solved()
        changed = self.app.update_capture(self.pid, project["captures"][0]["id"], {"enabled": False})
        self.assert_invalid_results(changed)
        self.assertFalse(changed["captures"][0]["enabled"])

    def test_threshold_only_change_keeps_frozen_k_and_requires_new_validation(self):
        project = self.solved()
        self.app.validate(self.pid)
        config = copy.deepcopy(project["config"])
        config["acceptance"]["intrinsics_p95_px"] = 0.2
        updated = self.app.update_config(self.pid, {"config": config})
        self.assertEqual(updated["model"]["K"], project["model"]["K"])
        self.assertEqual(updated["captures"], project["captures"])
        self.assertIsNone(updated["validation"])
        self.assertIsNone(updated["turntable"])

    def test_explicit_raw_coordinate_json_cannot_be_misread_as_oriented_points(self):
        project = self.demo()
        capture = copy.deepcopy(project["captures"][0])
        capture.update(name="qa-raw-coordinates.json", sequence_id="qa-raw-coordinate")
        capture["source"]["coordinate_domain"] = "raw_pixels"
        with self.assertRaises(ValueError):
            self.app.import_observations(self.pid, {"captures": [capture]})
        self.assertEqual(self.app.get_project(self.pid), project)

    def test_insufficient_poses_cannot_produce_model(self):
        project = self.demo()
        captures = project["captures"]
        for capture in [c for c in captures if c["split"] == "train"][5:]:
            self.app.update_capture(self.pid, capture["id"], {"enabled": False})
        with self.assertRaises(ValueError):
            self.app.solve(self.pid)
        self.assertIsNone(self.app.get_project(self.pid)["model"])

    def test_overlapping_sequence_is_rejected_before_it_can_be_validated(self):
        project = self.demo()
        train_id = next(c["sequence_id"] for c in project["captures"] if c["split"] == "train")
        observation = copy.deepcopy(next(c for c in project["captures"] if c["split"] == "test"))
        observation["id"] = "qa-overlap"
        observation["name"] = "qa-overlap.json"
        observation["sequence_id"] = train_id
        try:
            self.app.import_observations(self.pid, {"captures": [observation]})
        except ValueError:
            return
        with self.assertRaises(ValueError):
            self.app.solve(self.pid)

    def test_same_source_bytes_cannot_become_independent_by_renaming_sequence(self):
        from spike_calibration_tool.fixtures import demo_captures
        captures = demo_captures(self.project["config"])
        train = next(c for c in captures if c["split"] == "train")
        test = next(c for c in captures if c["split"] == "test")
        digest = hashlib.sha256(b"qa-identical-source-bytes").hexdigest()
        train["source"]["sha256"] = digest
        test["source"]["sha256"] = digest
        self.app.import_observations(self.pid, {"captures": captures})
        self.app.solve(self.pid)
        with self.assertRaises(ValueError):
            self.app.validate(self.pid)
        self.assertIsNone(self.app.get_project(self.pid)["validation"])

    def test_bad_file_in_batch_does_not_partially_save_first_capture(self):
        before = self.app.get_project(self.pid)
        with self.assertRaises((ValueError, TypeError)):
            self.app.import_files(self.pid, {
                "files": [png_file(), {"name": "bad.png", "content_base64": "not base64"}],
                "split": "train", "sequence_id": "qa-import-01", "evidence": "hardware",
            })
        self.assertEqual(self.app.get_project(self.pid), before)

    def test_nonfinite_observation_batch_cannot_change_project(self):
        demo = self.demo()
        first = copy.deepcopy(demo["captures"][0])
        first.update(id="qa-finite", name="qa-finite.json", sequence_id="qa-independent")
        bad = copy.deepcopy(first)
        bad.update(id="qa-nonfinite", name="qa-nonfinite.json")
        bad["image_points"][0][0] = float("nan")
        with self.assertRaises((ValueError, TypeError)):
            self.app.import_observations(self.pid, {"captures": [first, bad]})
        self.assertEqual(self.app.get_project(self.pid), demo)

    def test_provided_preview_must_decode_and_match_declared_size(self):
        project = self.demo()
        original = copy.deepcopy(project["captures"][0])
        original.update(name="qa-preview.json", sequence_id="qa-preview")
        for preview in (base64.b64encode(b"not an image").decode(), png_file()["content_base64"]):
            with self.subTest(preview_bytes=len(preview)):
                capture = copy.deepcopy(original)
                capture["preview_base64"] = preview
                with self.assertRaises(ValueError):
                    self.app.import_observations(self.pid, {"captures": [capture]})
                self.assertEqual(self.app.get_project(self.pid), project)

    def test_unknown_dat_layout_is_rejected_without_fabricated_dimensions(self):
        with self.assertRaises(ValueError):
            self.app.import_files(self.pid, {
                "files": [{"name": "sample.dat", "content_base64": base64.b64encode(bytes(32)).decode()}],
                "split": "train", "sequence_id": "qa-raw-01", "evidence": "hardware",
                "options": {},
            })
        self.assertEqual(self.app.get_project(self.pid)["captures"], [])

    def test_npz_mislabeled_as_npy_is_rejected_as_a_client_input_error(self):
        buffer = io.BytesIO()
        np.savez(buffer, spikes=np.zeros((4, 8, 8), dtype=np.uint8))
        with self.assertRaises(ValueError):
            self.app.import_files(self.pid, {
                "files": [{"name": "misnamed.npy", "content_base64": base64.b64encode(buffer.getvalue()).decode()}],
                "split": "train", "sequence_id": "qa-npz", "evidence": "public",
            })
        self.assertEqual(self.app.get_project(self.pid)["captures"], [])

    def test_sdk_roundtrip_matches_service_and_exposes_independent_config_copy(self):
        from spike_calibration_tool.sdk import SpikeCalibrationSDK
        project = self.solved()
        artifacts = self.root / "artifacts"
        artifacts.mkdir()
        path = artifacts / "calibration.json"
        path.write_text(json.dumps(self.app.export_project(self.pid), ensure_ascii=False, allow_nan=False), encoding="utf-8")
        sdk = SpikeCalibrationSDK(path)
        k = project["model"]["K"]
        points = [[k[0][2], k[1][2]], [k[0][2] + 30, k[1][2] - 20]]
        self.assertEqual(sdk.query(points), self.app.query(self.pid, {"points": points}))
        self.assertEqual(sdk.evidence, ["synthetic"])
        changed = sdk.config
        changed["orientation"]["flip_x"] = True
        self.assertFalse(sdk.config["orientation"]["flip_x"])
        path.write_text('{"format":"other","format_version":1}', encoding="utf-8")
        with self.assertRaises(ValueError):
            SpikeCalibrationSDK(path)

    def test_export_and_report_preserve_synthetic_scope_and_escape_project_name(self):
        solved = self.solved()
        exported = self.app.export_project(self.pid)
        if isinstance(exported, (bytes, str)):
            exported = json.loads(exported)
        encoded = json.dumps(exported, ensure_ascii=False, allow_nan=False)
        self.assertIn("synthetic", encoded)
        self.assertNotIn('"hardware_certified": true', encoded)
        html_project = self.app.create_project({"name": '<script>alert("qa")</script>'})
        report = self.app.report_html(html_project["id"])
        if isinstance(report, bytes):
            report = report.decode("utf-8")
        self.assertNotIn('<script>alert("qa")</script>', report)
        self.assertIn("&lt;script&gt;", report)

    def test_preview_is_real_png_and_unknown_capture_does_not_read_files(self):
        project = self.demo()
        png = self.app.preview(self.pid, project["captures"][0]["id"])
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        with self.assertRaises((ValueError, KeyError)):
            self.app.preview(self.pid, "../../context.md")


class HTTPAcceptanceTests(WorkbenchCase):
    def setUp(self):
        super().setUp()
        from spike_calibration_tool.server import CalibrationHTTPServer
        self.server = CalibrationHTTPServer(("127.0.0.1", 0), self.app)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": .05}, daemon=True)
        self.thread.start()
        self.origin = "http://127.0.0.1:" + str(self.server.server_address[1])

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()
        super().tearDown()

    def request(self, path, method="GET", payload=None, *, headers=None, raw=None):
        data = raw
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
        actual_headers = {"Content-Type": "application/json"} if data is not None else {}
        actual_headers.update(headers or {})
        request = Request(self.origin + path, data=data, method=method, headers=actual_headers)
        try:
            with urlopen(request, timeout=30) as response:
                return response.status, response.headers, response.read()
        except HTTPError as error:
            return error.code, error.headers, error.read()

    def test_actual_http_project_roundtrip_and_assets(self):
        status, _, body = self.request("/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["app"], "spike-calibration-tool")
        status, _, body = self.request("/api/projects", "POST", {"name": "HTTP 项目"})
        self.assertEqual(status, 200)
        project = json.loads(body)
        status, _, body = self.request("/api/projects/" + project["id"])
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["name"], "HTTP 项目")
        for path, mime in [("/", "text/html"), ("/app.js", "javascript"), ("/styles.css", "text/css")]:
            with self.subTest(asset=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertIn(mime, headers.get("Content-Type", ""))
                self.assertTrue(body)

    def test_cross_origin_and_unexpected_host_are_forbidden(self):
        for headers in ({"Origin": "https://outside.invalid"}, {"Host": "outside.invalid"},
                        {"Sec-Fetch-Site": "cross-site"}):
            with self.subTest(headers=headers):
                status, _, _ = self.request("/api/projects", "POST", {"name": "forbidden"}, headers=headers)
                self.assertEqual(status, 403)
        self.assertEqual(len(self.app.state()["projects"]), 1)

    def test_nonfinite_json_is_rejected_and_server_stays_usable(self):
        for raw in (b'{"name":"bad", "unexpected":NaN}', b'{"name":"bad", "unexpected":1e999}'):
            with self.subTest(raw=raw):
                status, _, _ = self.request("/api/projects", "POST", raw=raw)
                self.assertEqual(status, 400)
        status, _, body = self.request("/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(len(json.loads(body)["projects"]), 1)

    def test_non_json_write_and_malformed_json_are_client_errors(self):
        for raw, headers in [(b"name=oops", {"Content-Type": "text/plain"}), (b'{"name":', {})]:
            with self.subTest(raw=raw):
                status, _, _ = self.request("/api/projects", "POST", raw=raw, headers=headers)
                self.assertEqual(status, 400)

    def test_missing_resource_and_path_traversal_are_not_served(self):
        for path in ["/api/projects/missing", "/../context.md", "/%2e%2e/context.md"]:
            with self.subTest(path=path):
                status, _, _ = self.request(path)
                self.assertIn(status, (400, 404))


class DesktopBoundaryTests(unittest.TestCase):
    def test_skip_link_fragment_does_not_prevent_native_export(self):
        from spike_calibration_tool.desktop import DesktopBridge
        with tempfile.TemporaryDirectory(prefix="spike-desktop-qa-") as directory:
            destination = Path(directory) / "result.json"
            origin = "http://127.0.0.1:12345"
            route = "/api/projects/" + "a" * 32 + "/export"
            bridge = DesktopBridge(origin)
            # pywebview introspects public JS bridge members. Keeping native
            # window state private prevents recursion into Window/COM objects.
            self.assertFalse([name for name in vars(bridge) if not name.startswith("_")])
            bridge._window = types.SimpleNamespace(
                get_current_url=lambda: origin + "/#main",
                create_file_dialog=lambda *args, **kwargs: (str(destination),),
            )
            self.assertFalse([name for name in vars(bridge) if not name.startswith("_")])
            response = io.BytesIO(b'{"qa":"native export boundary"}')
            response.url = origin + route
            fake_webview = types.SimpleNamespace(FileDialog=types.SimpleNamespace(SAVE="save"))
            with patch.dict("sys.modules", {"webview": fake_webview}), patch("spike_calibration_tool.desktop.urlopen", return_value=response):
                result = bridge.save_export(route)
            self.assertNotIn("error", result)
            self.assertEqual(destination.read_bytes(), b'{"qa":"native export boundary"}')

    def test_winforms_save_chooser_runs_inside_ui_thread_invoke(self):
        from spike_calibration_tool.desktop import DesktopBridge

        class FakeAction:
            def __init__(self, callback):
                self.callback = callback

            def __call__(self):
                return self.callback()

        with tempfile.TemporaryDirectory(prefix="spike-native-dispatch-qa-") as directory:
            for save in (True, False):
                with self.subTest(save=save):
                    calls = {"inside_invoke": False, "invoke_count": 0, "chooser_count": 0}
                    destination = Path(directory) / ("saved.json" if save else "cancelled.json")

                    class FakeNative:
                        InvokeRequired = True

                        def Invoke(self, action):
                            self_case.assertIsInstance(action, FakeAction)
                            calls["invoke_count"] += 1
                            calls["inside_invoke"] = True
                            try:
                                action()
                            finally:
                                calls["inside_invoke"] = False

                    def choose(*args, **kwargs):
                        self.assertTrue(calls["inside_invoke"], "WinForms chooser must execute within native.Invoke")
                        calls["chooser_count"] += 1
                        return (str(destination),) if save else None

                    self_case = self
                    origin = "http://127.0.0.1:12345"
                    route = "/api/projects/" + "b" * 32 + "/export"
                    bridge = DesktopBridge(origin)
                    bridge._window = types.SimpleNamespace(
                        get_current_url=lambda: origin,
                        create_file_dialog=choose,
                        native=FakeNative(),
                    )
                    response = io.BytesIO(b'{"qa":"STA dispatch"}')
                    response.url = origin + route
                    modules = {
                        "webview": types.SimpleNamespace(FileDialog=types.SimpleNamespace(SAVE="save")),
                        "System": types.SimpleNamespace(Action=FakeAction),
                    }
                    with patch.dict("sys.modules", modules), patch("spike_calibration_tool.desktop.urlopen", return_value=response) as download:
                        result = bridge.save_export(route)
                    self.assertEqual(calls["invoke_count"], 1)
                    self.assertEqual(calls["chooser_count"], 1)
                    self.assertFalse(calls["inside_invoke"])
                    if save:
                        self.assertEqual(result, {"path": str(destination.resolve())})
                        self.assertEqual(destination.read_bytes(), b'{"qa":"STA dispatch"}')
                        download.assert_called_once_with(origin + route, timeout=120)
                    else:
                        self.assertIsNone(result)
                        self.assertFalse(destination.exists())
                        download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
