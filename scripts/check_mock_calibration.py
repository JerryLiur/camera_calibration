#!/usr/bin/env python
"""Run the mock data through the real calibration workbench and score the result.

Every scenario is imported over loopback HTTP into a fresh, temporary project
(the same API the browser workbench uses). The script then solves, validates,
fits the turntable, queries rays and exports a report, and compares each number
against ground_truth.json, which is never given to the software.

"passed" means the scenario behaved as designed. For the deliberately broken
scenarios (prism / degenerate_noisy / flat_distorted) a passed check means the
test successfully exposed a limitation; it does NOT mean the calibration is good.

Usage
-----
    python scripts/check_mock_calibration.py --data output/mock_calibration_data
    python scripts/check_mock_calibration.py --only clean,png
    python scripts/check_mock_calibration.py --skip-binary
"""
from __future__ import annotations

import argparse
import base64
import json
import math
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import numpy as np

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

REJECT_ON_IMPORT = {"window_limit"}
REJECT_ON_SOLVE = {"degenerate", "degenerate_noisy", "flat_distorted", "leak"}
UNSTABLE_SOLVE = set()
LINK_ONLY = {"spike_npy", "packed_dat"}


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")


class Harness:
    """Temporary workbench + loopback HTTP server; never touches user projects."""

    def __init__(self):
        from spike_calibration_tool.server import CalibrationHTTPServer
        from spike_calibration_tool.service import Workbench
        self._tmp = tempfile.TemporaryDirectory(prefix="spike-mock-check-")
        self.app = Workbench(self._tmp.name)
        self.server = CalibrationHTTPServer(("127.0.0.1", 0), self.app)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        self.origin = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.calls = 0

    def request(self, path, method="GET", payload=None, *, expect=200, binary=False,
                allow_error=False):
        headers = {"Content-Type": "application/json"} if payload is not None else {}
        body = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode() if payload is not None else None
        req = Request(self.origin + path, data=body, method=method, headers=headers)
        try:
            with urlopen(req, timeout=180) as response:
                status, content = response.status, response.read()
        except HTTPError as exc:
            status, content = exc.code, exc.read()
        self.calls += 1
        parsed = content if binary else json.loads(content)
        if allow_error:
            return status, parsed
        if status != expect:
            raise AssertionError(f"{method} {path}: expected {expect}, got {status}: {content[:400]!r}")
        return parsed

    def close(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()
        self._tmp.cleanup()


def check(name, ok, detail) -> dict:
    return {"name": name, "passed": bool(ok), "detail": detail}


def load_payload(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def import_scenario(h: Harness, prefix: str, scene: dict, root: Path) -> dict:
    """Import one scenario; returns per-file diagnostics and any expected rejection."""
    imported, rejection = [], None
    for item in scene["files"]:
        path = root / item["path"]
        if scene["kind"] == "observations":
            payload = load_payload(path)
            label = f"{item['path']} ({len(payload['captures'])} 组观测)"
        else:
            raw = path.read_bytes()
            payload = {"files": [{"name": path.name,
                                  "content_base64": base64.b64encode(raw).decode("ascii")}],
                       "split": item["split"], "sequence_id": item["sequence_id"],
                       "evidence": "synthetic", "options": item.get("options", {})}
            label = f"{path.name} ({len(raw) / 1024 ** 2:.1f} MiB)"
        endpoint = "/observations" if scene["kind"] == "observations" else "/import"
        status, response = h.request(prefix + endpoint, "POST", payload, allow_error=True)
        if status == 200:
            imported.append({"file": item["path"], "label": label, "accepted": True})
        else:
            error = response.get("error", json.dumps(response, ensure_ascii=False)) if isinstance(response, dict) else str(response)
            if scene["id"] not in REJECT_ON_IMPORT:
                raise AssertionError(f"导入 {item['path']} 意外失败 ({status}): {error}")
            rejection = {"file": item["path"], "error": error}
            imported.append({"file": item["path"], "label": label, "accepted": False, "error": error})
    return {"imported": imported, "rejection": rejection}


def solve_error(h: Harness, prefix: str) -> str:
    status, response = h.request(prefix + "/solve", "POST", {}, allow_error=True)
    if status == 200:
        return "UNEXPECTED SUCCESS: 退化数据被接受"
    return response.get("error", "") if isinstance(response, dict) else str(response)


def corner_quality(state, truth_by_index) -> dict:
    found, rms_values, counts = 0, [], []
    for capture in state["captures"]:
        if not capture["found"]:
            continue
        found += 1
        counts.append(len(capture["image_points"]))
        try:
            index = int(Path(capture["name"]).stem.split("_")[-1])
        except ValueError:
            continue
        gt = truth_by_index.get(index)
        if gt is None:
            continue
        det = np.asarray(capture["image_points"], float)
        if det.shape == gt.shape:
            rms_values.append(float(np.sqrt(np.mean(np.sum((det - gt) ** 2, axis=1)))))
    return {"found": found, "total": len(state["captures"]),
            "corner_counts": counts,
            "corner_rms_px": float(np.mean(rms_values)) if rms_values else None}


def nonfinite_paths(obj, path="model") -> list[str]:
    """Find every non-finite number in a nested solver result."""
    found = []
    if isinstance(obj, dict):
        for key, value in obj.items():
            found.extend(nonfinite_paths(value, f"{path}.{key}"))
    elif isinstance(obj, (list, tuple)):
        for index, value in enumerate(obj):
            found.extend(nonfinite_paths(value, f"{path}[{index}]"))
    elif isinstance(obj, (int, float, np.number)) and not isinstance(obj, bool):
        try:
            if not np.isfinite(obj):
                found.append(path)
        except (TypeError, ValueError):
            pass
    return found


def solver_probe(scene: dict, root: Path, config: dict, repeats: int = 3) -> dict:
    """Call the numerical solver directly, bypassing HTTP serialisation.

    The extreme-degeneracy case can make OpenCV return NaN in intrinsic_std;
    repeating the call documents that the result is not even deterministic.
    """
    from spike_calibration_tool.engine import solve_intrinsics, validate_config
    captures = []
    for item in scene["files"]:
        captures.extend(load_payload(root / item["path"])["captures"])
    cfg = validate_config(config)
    runs = []
    for _ in range(repeats):
        try:
            model = solve_intrinsics(captures, cfg)
            paths = nonfinite_paths(model)
            runs.append({"fx_px": float(model["K"][0][0]),
                         "fit_rms_px": float(model["diagnostics"]["fit_rms_px"]),
                         "nonfinite_fields": paths, "json_serialisable": not paths})
        except Exception as exc:  # noqa: BLE001
            runs.append({"error": repr(exc)})
    return {"repeats": repeats, "runs": runs,
            "note": "旁路调用求解器以区分『数值发散』与『HTTP 序列化失败』"}


def score_unstable(probe: dict, solve_rejection: str | None, unexpected_solve: dict | None,
                   truth: dict) -> list[dict]:
    runs = probe["runs"]
    fx = [r["fx_px"] for r in runs if "fx_px" in r]
    bad_fields = [p for r in runs for p in r.get("nonfinite_fields", [])]
    truth_fx = truth["K"][0][0]
    http_text = (f"HTTP 400：{solve_rejection}" if solve_rejection else
                 "HTTP 200：返回了荒谬但可序列化的模型" if unexpected_solve else "未记录")
    return [
        check("极端退化输入未被姿态拦截", True, "0.04 px 噪声即可绕过单应秩亏检查"),
        check("焦距偏离真值 > 100 px（结果不可用）",
              bool(fx) and max(abs(f - truth_fx) for f in fx) > 100,
              f"fx={[round(f, 1) for f in fx]}，真值 {truth_fx}"),
        check("重投影残差仍 < 0.15 px（自洽≠正确）",
              all(r.get("fit_rms_px", 1) < 0.15 for r in runs if "fit_rms_px" in r),
              ", ".join(f"{r['fit_rms_px']:.4f}" for r in runs if "fit_rms_px" in r)),
        check("诊断数组出现 NaN 或求解器数值不稳定",
              bool(bad_fields) or len(set(round(f, 3) for f in fx)) > 1 or bool(solve_rejection),
              f"非有限字段={sorted(set(bad_fields))[:4]}，重复调用 fx 种类={len(set(round(f, 3) for f in fx))}"),
        check("HTTP 层行为已记录", bool(solve_rejection or unexpected_solve), http_text),
    ]


def compare_to_truth(model, validation, rays, turntables, truth) -> dict:
    K = np.asarray(model["K"], float)
    Kt = np.asarray(truth["K"], float)
    D = np.asarray(model["distortion"], float).reshape(-1)
    Dt = np.asarray(truth["distortion"], float).reshape(-1)
    expected = truth["query_rays"]
    az = np.array([r["azimuth_deg"] - e["azimuth_deg"] for r, e in zip(rays["points"], expected)])
    el = np.array([r["elevation_deg"] - e["elevation_deg"] for r, e in zip(rays["points"], expected)])
    angle_errors = np.abs(np.concatenate((az, el)))
    zero = truth["turntable"]["zero_offset_deg"]
    result = {
        "delta_fx_px": float(K[0, 0] - Kt[0, 0]),
        "delta_fy_px": float(K[1, 1] - Kt[1, 1]),
        "delta_cx_px": float(K[0, 2] - Kt[0, 2]),
        "delta_cy_px": float(K[1, 2] - Kt[1, 2]),
        "delta_distortion": (D - Dt).tolist(),
        "fit_rms_px": float(model["diagnostics"]["fit_rms_px"]),
        "coverage_fraction": float(model["diagnostics"]["coverage"]["coverage_fraction"]),
        "validation_p95_px": float(validation["p95_px"]),
        "validation_status": validation["status"],
        "hardware_qualified": validation["hardware_qualified"],
        "ray_max_abs_error_deg": float(angle_errors.max()),
        "ray_rms_error_deg": float(np.sqrt(np.mean(angle_errors ** 2))),
        "warnings": model["warnings"],
        "turntable": {},
    }
    for mode, tt in turntables.items():
        value = float(tt["zero_offset_deg"])
        result["turntable"][mode] = {"zero_offset_deg": value,
                                     "error_deg": float(value - zero),
                                     "status": tt["status"],
                                     "test_p95_deg": float(tt["test_p95_deg"])}
    return result


def score_accuracy(m: dict) -> list[dict]:
    return [
        check("焦距误差 < 3 px", max(abs(m["delta_fx_px"]), abs(m["delta_fy_px"])) < 3,
              f"dFx={m['delta_fx_px']:+.3f}, dFy={m['delta_fy_px']:+.3f}"),
        check("主点误差 < 1.5 px", max(abs(m["delta_cx_px"]), abs(m["delta_cy_px"])) < 1.5,
              f"dCx={m['delta_cx_px']:+.3f}, dCy={m['delta_cy_px']:+.3f}"),
        check("训练重投影 RMS < 0.15 px", m["fit_rms_px"] < 0.15, f"{m['fit_rms_px']:.4f} px"),
        check("独立验证 P95 < 0.25 px 且 passed",
              m["validation_p95_px"] < 0.25 and m["validation_status"] == "passed",
              f"P95={m['validation_p95_px']:.4f} px, status={m['validation_status']}"),
        check("视线角最大误差 < 0.2°", m["ray_max_abs_error_deg"] < 0.2,
              f"{m['ray_max_abs_error_deg']:.5f}°"),
        check("视场覆盖 100%", m["coverage_fraction"] >= 0.999, f"{m['coverage_fraction']:.3f}"),
        check("转台零位误差 < 0.15°（两种模式）",
              all(abs(t["error_deg"]) < 0.15 for t in m["turntable"].values()),
              ", ".join(f"{k}={v['error_deg']:+.4f}°" for k, v in m["turntable"].items())),
        check("未伪装成实机合格", m["hardware_qualified"] is False,
              f"hardware_qualified={m['hardware_qualified']}"),
    ]


def score(scene_id: str, metrics: dict | None, quality: dict | None,
          rejection: dict | None, solve_rejection: str | None,
          export_bundle: dict | None, scene_notes: dict) -> list[dict]:
    note = scene_notes.get(scene_id, {})
    quality = quality or {"found": 0, "total": 0, "corner_counts": [], "corner_rms_px": None}
    rms_text = f"{quality['corner_rms_px']:.3f} px" if quality["corner_rms_px"] is not None else "无匹配真值"
    if scene_id in REJECT_ON_IMPORT:
        text = rejection["error"] if rejection else ""
        return [check("导入被拒绝", rejection is not None, text or "未被拒绝"),
                check("拒绝原因指向窗口/元素上限", "时间窗" in text or "一亿" in text, text)]
    if scene_id in REJECT_ON_SOLVE:
        text = solve_rejection or ""
        keyword = "相同的角点观测" if scene_id == "leak" else "姿态约束退化"
        return [check("求解被拒绝", bool(text) and not text.startswith("UNEXPECTED"), text),
                check(f"拒绝原因包含『{keyword}』", keyword in text, text)]
    if scene_id in LINK_ONLY:
        expected = {"spike_npy": 3, "packed_dat": 1}[scene_id]
        return [check(f"{expected}/{expected} 文件检出靶标", quality and quality["found"] == expected,
                      json.dumps(quality, ensure_ascii=False)),
                check("每张 54 个角点", all(n == 54 for n in quality["corner_counts"]),
                      str(quality["corner_counts"])),
                check("检测角点 RMS < 1.5 px", quality["corner_rms_px"] is not None and quality["corner_rms_px"] < 1.5,
                      rms_text)]
    m = metrics
    if scene_id == "clean":
        result = score_accuracy(m)
        result.append(check("导出包未认证实机合格",
                            export_bundle is not None and export_bundle.get("hardware_certified") is False,
                            f"hardware_certified={export_bundle.get('hardware_certified') if export_bundle else 'n/a'}"))
        return result
    if scene_id == "noisy":
        return [check("噪声使 RMS 明显上升 (>0.3 px)", m["fit_rms_px"] > 0.3, f"{m['fit_rms_px']:.4f} px"),
                check("独立验证 P95 超过 0.5 px 阈值", m["validation_p95_px"] > 0.5, f"{m['validation_p95_px']:.4f} px"),
                check("验收门禁判为 failed", m["validation_status"] == "failed", m["validation_status"]),
                check("内参仍保持可解 (|dFx| < 15 px)", abs(m["delta_fx_px"]) < 15, f"{m['delta_fx_px']:+.3f} px")]
    if scene_id == "prism":
        return [check("重投影残差仍然很小 (<0.15 px)", m["fit_rms_px"] < 0.15, f"{m['fit_rms_px']:.4f} px"),
                check("独立验证仍判 passed", m["validation_status"] == "passed", m["validation_status"]),
                check("但视线指向误差 > 0.25°（暴露模型失配）", m["ray_max_abs_error_deg"] > 0.25,
                      f"{m['ray_max_abs_error_deg']:.4f}°"),
                check("主点被畸变带动 > 3 px", abs(m["delta_cx_px"]) > 3, f"dCx={m['delta_cx_px']:+.3f} px")]
    if scene_id == "flat_distorted":
        threshold = 20
        return [check("求解未被拦截（记录防护盲区）", True, "软件接受了姿态不足的数据"),
                check(f"焦距误差 > {threshold} px（结果不可用）", abs(m["delta_fx_px"]) > threshold,
                      f"dFx={m['delta_fx_px']:+.2f} px"),
                check("但重投影 RMS 仍 < 0.15 px（自洽≠正确）", m["fit_rms_px"] < 0.15,
                      f"{m['fit_rms_px']:.4f} px"),
                check("视线误差 > 0.5°", m["ray_max_abs_error_deg"] > 0.5,
                      f"{m['ray_max_abs_error_deg']:.4f}°")]
    if scene_id == "png":
        return [check("12/12 张图检出靶标", quality["found"] == 12, str(quality)),
                check("每张 54 个角点", all(n == 54 for n in quality["corner_counts"]), str(quality["corner_counts"])),
                check("检测角点 RMS < 1.5 px", quality["corner_rms_px"] is not None and quality["corner_rms_px"] < 1.5, rms_text),
                check("求解与独立验证流程可执行", m["validation_status"] in ("passed", "failed", "measured"),
                      f"status={m['validation_status']}，P95={m['validation_p95_px']:.4f} px"),
                check("栅格化误差下焦距偏差 < 20 px", abs(m["delta_fx_px"]) < 20, f"{m['delta_fx_px']:+.3f} px"),
                check("图像检测链的指向误差明显大于理想角点 (>0.5°)",
                      m["ray_max_abs_error_deg"] > 0.5, f"{m['ray_max_abs_error_deg']:.4f}°")]
    return [check("场景已执行", True, note.get("expected", ""))]


def run_scenario(h: Harness, scene: dict, root: Path, truth: dict, config: dict,
                 output: Path, notes: dict) -> dict:
    scene_id = scene["id"]
    project = h.request("/api/projects", "POST", {"name": f"模拟测试 · {scene_id}"})
    prefix = f"/api/projects/{project['id']}"
    h.request(prefix + "/config", "PATCH", {"config": config})

    imported = import_scenario(h, prefix, scene, root)
    state = h.request(prefix)
    truth_by_index = {p["index"]: np.asarray(p["image_points_truth"], float) for p in truth["poses"]}
    quality = corner_quality(state, truth_by_index) if scene["kind"] == "files" else None

    metrics, export_bundle, solve_rejection = None, None, None
    probe, unexpected_solve = None, None
    if scene_id in REJECT_ON_IMPORT:
        pass
    elif scene_id in REJECT_ON_SOLVE:
        solve_rejection = solve_error(h, prefix)
    elif scene_id in UNSTABLE_SOLVE:
        status, response = h.request(prefix + "/solve", "POST", {}, allow_error=True)
        if status == 200:
            unexpected_solve = response
        else:
            solve_rejection = response.get("error", "") if isinstance(response, dict) else str(response)
        probe = solver_probe(scene, root, config)
    elif scene_id in LINK_ONLY:
        # Only three / one views: these scenarios verify the import + detection
        # pipeline, not a full calibration (which needs >= 6 distinct poses).
        pass
    else:
        solved = h.request(prefix + "/solve", "POST", {})
        model = solved["model"]
        validated = h.request(prefix + "/validate", "POST", {})
        rays = h.request(prefix + "/query", "POST",
                         {"points": [p["pixel"] for p in truth["query_rays"]]})
        turntables = {}
        for mode in ("far_field", "finite_distance"):
            payload = truth["turntable"][mode]
            result = h.request(prefix + "/turntable", "POST",
                               {"observations": payload["observations"],
                                "reference": payload["reference"]})
            turntables[mode] = result["turntable"]
        metrics = compare_to_truth(model, validated["validation"], rays, turntables, truth)
        if scene_id == "clean":
            export_bundle = h.request(prefix + "/export")
            write_json(output / "bundle_clean.json", export_bundle)
            (output / "report_clean.html").write_bytes(h.request(prefix + "/report", binary=True))
            write_json(output / "model_clean.json", model)
            write_json(output / "validation_clean.json", validated["validation"])

    if scene_id in UNSTABLE_SOLVE:
        checks = score_unstable(probe, solve_rejection, unexpected_solve, truth)
    else:
        checks = score(scene_id, metrics, quality, imported["rejection"], solve_rejection,
                       export_bundle, notes)
    return {"id": scene_id, "kind": scene["kind"], "purpose": notes.get(scene_id, {}).get("purpose", ""),
            "expected": notes.get(scene_id, {}).get("expected", ""),
            "project_id": project["id"], "imports": imported["imported"],
            "corner_quality": quality, "metrics": metrics,
            "import_rejection": imported["rejection"], "solve_rejection": solve_rejection,
            "solver_probe": probe,
            "http_solve_status": (400 if solve_rejection else 200) if scene_id not in REJECT_ON_IMPORT else None,
            "checks": checks, "passed": all(c["passed"] for c in checks)}


def summary_text(report: dict) -> str:
    lines = ["=" * 78,
             "Spike 标定工具 · 模拟数据闭环检查",
             f"数据目录 : {report['data_dir']}",
             f"软件版本 : {report['software_version']}   HTTP 调用 : {report['http_calls']}   耗时 : {report['elapsed_s']} s",
             "=" * 78,
             f"真值 K  : fx={report['truth_K'][0][0]}, fy={report['truth_K'][1][1]}, "
             f"cx={report['truth_K'][0][2]}, cy={report['truth_K'][1][2]}",
             f"真值畸变: {[round(x, 5) for x in report['truth_distortion']]}",
             "",
             f"{'场景':<18}{'结果':<8}{'关键指标'}",
             "-" * 78]
    for scene in report["scenarios"]:
        m, q = scene["metrics"], scene["corner_quality"]
        if scene["import_rejection"]:
            key = f"导入被拒：{scene['import_rejection']['error'][:44]}"
        elif scene.get("solver_probe"):
            runs = scene["solver_probe"]["runs"]
            fx = [r["fx_px"] for r in runs if "fx_px" in r]
            nan_runs = sum(bool(r.get("nonfinite_fields")) for r in runs)
            key = (f"旁路探针 fx={[round(f) for f in fx]}，NaN {nan_runs}/{len(runs)} 次，"
                   f"HTTP={'400 序列化失败' if scene['solve_rejection'] else '200 荒谬参数'}")
        elif scene["solve_rejection"]:
            key = f"求解被拒：{scene['solve_rejection'][:44]}"
        elif m:
            key = (f"dFx={m['delta_fx_px']:+.2f}px  fit={m['fit_rms_px']:.4f}  "
                   f"P95={m['validation_p95_px']:.4f}  指向误差={m['ray_max_abs_error_deg']:.4f}°  "
                   f"零位误差={m['turntable']['far_field']['error_deg']:+.4f}°")
        elif q:
            rms = f"{q['corner_rms_px']:.3f} px" if q["corner_rms_px"] is not None else "—"
            key = f"检出 {q['found']}/{q['total']}，角点 RMS {rms}"
        else:
            key = "（无图像链路指标）"
        lines.append(f"{scene['id']:<18}{('PASS' if scene['passed'] else 'FAIL'):<8}{key}")
        for c in scene["checks"]:
            lines.append(f"    {'OK ' if c['passed'] else '!! '}{c['name']}：{c['detail']}")
    lines += ["-" * 78,
              f"总计：{sum(s['passed'] for s in report['scenarios'])}/{len(report['scenarios'])} 个场景符合预期",
              "说明：PASS 表示软件行为符合该模拟用例的设计预期；退化场景须被拒绝。",
              "      prism 的 PASS 表示暴露了模型失配局限，不代表该标定结果可用。",
              "      所有数据来源均为 synthetic，导出包不会认证实机合格。",
              "=" * 78]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="把模拟数据输入标定软件并对照已知真值检查")
    parser.add_argument("--data", type=Path, default=REPO / "output/mock_calibration_data")
    parser.add_argument("--output", type=Path, default=REPO / "output/mock_calibration_check")
    parser.add_argument("--skip-binary", action="store_true", help="跳过 NPY / DAT 场景")
    parser.add_argument("--only", type=str, default="", help="只运行逗号分隔的场景 ID")
    args = parser.parse_args()

    root = args.data.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = load_payload(root / "manifest.json")
    truth = load_payload(root / "ground_truth.json")
    config = manifest["software_config"]
    notes = manifest.get("scenario_notes", {})
    selected = set(filter(None, args.only.split(","))) if args.only else None
    scenes = [s for s in manifest["scenarios"]
              if (not selected or s["id"] in selected)
              and not (args.skip_binary and s["id"] in {"spike_npy", "packed_dat", "window_limit"})]

    harness = Harness()
    start = time.monotonic()
    results = []
    try:
        version = harness.request("/api/state")["version"]
        for scene in scenes:
            result = run_scenario(harness, scene, root, truth, config, output, notes)
            results.append(result)
            print(f"[{'PASS' if result['passed'] else 'FAIL'}] {scene['id']}", flush=True)
    finally:
        calls = harness.calls
        harness.close()
    report = {
        "schema": "spike-mock-calibration-check/1",
        "data_dir": str(root), "output_dir": str(output), "software_version": version,
        "http_calls": calls, "elapsed_s": round(time.monotonic() - start, 2),
        "truth_K": truth["K"], "truth_distortion": truth["distortion"],
        "software_config": config,
        "scenarios": results,
        "overall_passed": all(r["passed"] for r in results),
        "limitations": ["全部数据为合成数据，不能证明实机精度。",
                        "重投影残差只检验几何自洽；绝对指向误差需独立计量真值。",
                        "Spike 脉冲为简化伯努利模型，不代表真实传感器时序特性。"],
    }
    write_json(output / "check_report.json", report)
    text = summary_text(report)
    (output / "summary.txt").write_text(text + "\n", encoding="utf-8")
    print("\n" + text)
    print(json.dumps({"overall_passed": report["overall_passed"],
                      "scenarios": len(results), "http_calls": calls,
                      "output": str(output)}, ensure_ascii=False))
    return 0 if report["overall_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
