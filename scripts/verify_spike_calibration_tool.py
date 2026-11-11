"""Run a real HTTP calibration journey and preserve evidence, without a browser.

Usage: .venv/Scripts/python.exe scripts/verify_spike_calibration_tool.py
The run owns a temporary workspace/ephemeral HTTP port. Saved outputs are QA
artifacts only; they never change the user's actual calibration workspace.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
import tempfile
import threading
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import cv2
import numpy as np

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")


def turntable_observations(model, *, mode="far_field", zero_deg=1.35):
    """Forward-project analytically known geometry, independent of tool fitting."""
    target = np.array([0.30, 0.04, 1.75])
    offset = np.array([0.12, 0.01, 0.06])
    k, d = np.asarray(model["K"], float), np.asarray(model["distortion"], float)
    records = []
    for split, encoders in [("train", [-9., -5., 0., 4., 9.]), ("test", [-7., 2., 7.])]:
        for i, encoder in enumerate(encoders):
            if mode == "far_field":
                angle = -encoder - zero_deg
            else:
                q = math.radians(-encoder)
                rotation = np.array([[math.cos(q), 0., math.sin(q)], [0., 1., 0.],
                                     [-math.sin(q), 0., math.cos(q)]])
                point_body = rotation @ target - offset
                angle = math.degrees(math.atan2(point_body[0], point_body[2])) - zero_deg
            ray = np.array([[math.tan(math.radians(angle)), 0., 1.]])
            pixel, _ = cv2.projectPoints(ray, np.zeros(3), np.zeros(3), k, d)
            records.append({"sequence_id": "qa-turntable-" + split, "split": split,
                            "encoder_deg": encoder, "pixel": pixel.reshape(2).tolist()})
    reference = {"mode": mode, "sign": 1}
    if mode == "far_field":
        reference.update(reference_angle_deg=0., far_field_confirmed=True)
    else:
        reference.update(target_position_m=target.tolist(), camera_offset_m=offset.tolist())
    return {"observations": records, "reference": reference}


def run(output):
    from spike_calibration_tool.server import CalibrationHTTPServer
    from spike_calibration_tool.service import Workbench
    output.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    steps = []
    with tempfile.TemporaryDirectory(prefix="spike-http-acceptance-") as data:
        app = Workbench(data)
        server = CalibrationHTTPServer(("127.0.0.1", 0), app)
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .05}, daemon=True)
        thread.start()
        origin = f"http://127.0.0.1:{server.server_address[1]}"

        def request(path, method="GET", payload=None, *, expect=200, binary=False):
            headers = {"Content-Type": "application/json"} if payload is not None else {}
            body = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode() if payload is not None else None
            req = Request(origin + path, data=body, method=method, headers=headers)
            try:
                with urlopen(req, timeout=90) as response:
                    status, content, mime = response.status, response.read(), response.headers.get("Content-Type", "")
            except HTTPError as exc:
                status, content, mime = exc.code, exc.read(), exc.headers.get("Content-Type", "")
            steps.append({"method": method, "path": path, "status": status, "content_type": mime})
            if status != expect:
                raise AssertionError(f"{method} {path}: expected {expect}, got {status}: {content[:1000]!r}")
            return content if binary else json.loads(content)

        try:
            state = request("/api/state")
            assert state["capabilities"]["hardware_connected"] is False
            project = request("/api/projects", "POST", {"name": "QA HTTP 完整标定"})
            prefix = "/api/projects/" + project["id"]
            project = request(prefix + "/demo", "POST", {})
            assert len(project["captures"]) >= 8
            assert {c["evidence"] for c in project["captures"]} == {"synthetic"}
            write_json(output / "demo_observations.json", {"captures": project["captures"]})
            project = request(prefix + "/solve", "POST", {})
            k = np.asarray(project["model"]["K"])
            assert k.shape == (3, 3) and np.isfinite(k).all()
            assert max(abs(k[0, 0] - 620), abs(k[1, 1] - 610)) < 3, k
            write_json(output / "solved_project.json", project)
            frozen = k.copy()
            project = request(prefix + "/validate", "POST", {})
            assert np.array_equal(np.asarray(project["model"]["K"]), frozen)
            assert project["validation"]["status"] == "measured", project["validation"]
            write_json(output / "validation.json", project["validation"])
            ray = request(prefix + "/query", "POST", {"points": [[float(k[0, 2]), float(k[1, 2])]]})
            assert abs(ray["points"][0]["azimuth_deg"]) < 1e-8
            assert abs(ray["points"][0]["elevation_deg"]) < 1e-8
            write_json(output / "ray_query.json", ray)
            for mode in ("far_field", "finite_distance"):
                body = turntable_observations(project["model"], mode=mode)
                project = request(prefix + "/turntable", "POST", body)
                tt = project["turntable"]
                zero = tt.get("zero_offset_deg", tt.get("zero_deg"))
                assert zero is not None and abs(zero - 1.35) < 1e-4, tt
                write_json(output / f"turntable_{mode}.json", {"input": body, "result": tt})
            cap_id = project["captures"][0]["id"]
            for suffix, filename in [("", "capture.png"), ("?undistort=1", "capture_undistorted.png")]:
                binary = request(prefix + "/preview/" + cap_id + suffix, binary=True)
                assert binary.startswith(b"\x89PNG\r\n\x1a\n")
                (output / filename).write_bytes(binary)
            exported = request(prefix + "/export")
            assert exported["hardware_certified"] is False
            assert exported["evidence"] == ["synthetic"]
            write_json(output / "bundle.json", exported)
            report = request(prefix + "/report", binary=True)
            assert b"<!doctype html>" in report.lower()
            (output / "report.html").write_bytes(report)
            persisted = Workbench(data).get_project(project["id"])
            assert persisted["model"]["K"] == project["model"]["K"]
            assert persisted["turntable"] == project["turntable"]
            changed = request(prefix + "/captures/" + cap_id, "PATCH", {"enabled": False})
            assert changed["model"] is None and changed["validation"] is None and changed["turntable"] is None
            request(prefix + "/query", "POST", {"points": [[20, 20]]}, expect=400)
            summary = {"passed": True, "evidence": "synthetic_software_validation",
                       "elapsed_s": round(time.monotonic() - start, 3), "http_steps": steps,
                       "limitations": ["No physical Spike camera or turntable was connected.",
                                       "No measured hardware precision or browser result is asserted here."]}
            write_json(output / "verification.json", summary)
            return summary
        except Exception as exc:
            write_json(output / "verification.json", {"passed": False, "error": repr(exc), "http_steps": steps})
            raise
        finally:
            server.shutdown()
            thread.join(timeout=5)
            server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=REPO / "output/http-verification")
    args = parser.parse_args()
    result = run(args.output.resolve())
    print(json.dumps({"passed": result["passed"], "http_steps": len(result["http_steps"]),
                      "elapsed_s": result["elapsed_s"], "output": str(args.output.resolve())}, ensure_ascii=False))
