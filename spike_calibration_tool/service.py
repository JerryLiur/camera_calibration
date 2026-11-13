"""Persistent projects for the Spike-only workbench; numerical work lives in engine."""
from __future__ import annotations

import base64
import copy
import hashlib
import html
import json
import math
import os
import re
import threading
import uuid
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

from . import __version__, engine


APP_ID = "spike-calibration-tool"
DEFAULT_CONFIG = {
    "target": {"kind": "chessboard", "cols": 9, "rows": 6, "square_length_m": 0.025},
    "orientation": {"rotation_deg": 0, "flip_x": False, "flip_y": False},
    "optics": {"shared_lens": True, "split_ratio": "1:1", "lens_id": "", "focus_note": "", "splitter_note": ""},
    "acceptance": {"intrinsics_p95_px": None, "turntable_p95_deg": None},
    "operator": "",
}


def dumps(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, indent=2)


def strict_loads(value):
    def invalid_constant(constant):
        raise ValueError("JSON 不允许非有限数值：" + constant)
    def finite_float(number):
        result = float(number)
        if not math.isfinite(result):
            raise ValueError("JSON 数值超出有限范围")
        return result
    return json.loads(value, parse_constant=invalid_constant, parse_float=finite_float)


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def fingerprint(config):
    geometry = {k: config[k] for k in ("target", "orientation", "optics")}
    return hashlib.sha256(json.dumps(geometry, sort_keys=True, allow_nan=False).encode()).hexdigest()


def validate_project(project):
    """Validate persisted state before exposing it to the UI or SDK.

    Keep a corrupt project on disk for recovery, but never allow it to poison
    every project's state endpoint or silently reuse a differently oriented K.
    """
    if not isinstance(project, dict):
        raise ValueError("工程必须为对象")
    required = {"id", "name", "config", "captures", "model", "validation",
                "turntable", "revision", "status", "updated_at"}
    if not required.issubset(project):
        raise ValueError("工程缺少必要字段")
    if not isinstance(project["name"], str) or not project["name"].strip() or len(project["name"]) > 120:
        raise ValueError("工程名称无效")
    if type(project["revision"]) is not int or project["revision"] < 1:
        raise ValueError("工程版本无效")
    if project["status"] not in ("empty", "collecting", "solved", "validated") or not isinstance(project["updated_at"], str):
        raise ValueError("工程状态无效")
    project["config"] = engine.validate_config(project["config"])
    if not isinstance(project["captures"], list) or len(project["captures"]) > 300:
        raise ValueError("工程观测列表无效")
    project["captures"] = [engine.validate_capture(c) for c in project["captures"]]
    if len({c["id"] for c in project["captures"]}) != len(project["captures"]):
        raise ValueError("工程观测 ID 重复")
    for key in ("model", "validation", "turntable"):
        if project[key] is not None and not isinstance(project[key], dict):
            raise ValueError(f"工程 {key} 无效")
    model = project["model"]
    if model is not None:
        engine._camera(model)
        if model.get("config_fingerprint") != fingerprint(project["config"]):
            raise ValueError("内参模型与工程光学/方向配置不一致，请重新标定")
    elif project["validation"] is not None or project["turntable"] is not None or project["status"] in ("solved", "validated"):
        raise ValueError("工程缺少结果对应的内参模型")
    return project


@contextmanager
def project_write_lock(path):
    """OS-owned lock: serialise revision check and replacement across processes."""
    with path.open("a+b") as stream:
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ValueError("该工程正在另一窗口保存，请刷新后重试") from exc
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


class Workbench:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.projects = {}
        self.load_errors = []
        for file in self.root.glob("*.json"):
            try:
                project = strict_loads(file.read_text(encoding="utf-8"))
                if not isinstance(project, dict) or project.get("id") != file.stem:
                    raise ValueError("project identity mismatch")
                self._check_id(project["id"])
                dumps(project)
                validate_project(project)
                self.projects[project["id"]] = project
            except (ValueError, TypeError, KeyError, OSError) as exc:
                self.load_errors.append(f"{file.name}: {exc}")

    @staticmethod
    def _check_id(value):
        if not isinstance(value, str) or not re.fullmatch(r"[a-f0-9]{32}", value):
            raise KeyError("工程不存在")

    def _project(self, project_id):
        self._check_id(project_id)
        return copy.deepcopy(self.projects[project_id])

    def _save(self, project):
        with project_write_lock(self.root / (project["id"] + ".py-lock")):
            return self._save_locked(project)

    def _save_locked(self, project):
        target = self.root / (project["id"] + ".json")
        if target.is_file():
            stored = strict_loads(target.read_text(encoding="utf-8"))
            validate_project(stored)
            if stored.get("revision") != project["revision"]:
                self.projects[project["id"]] = stored
                raise ValueError("该工程已在另一窗口修改，请刷新后重试")
        project["revision"] += 1
        project["updated_at"] = now()
        encoded = dumps(project)
        temp = target.with_suffix(".tmp")
        with temp.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, target)
        self.projects[project["id"]] = copy.deepcopy(project)
        return copy.deepcopy(project)

    @staticmethod
    def _invalidate(project):
        project.update(model=None, validation=None, turntable=None, status="collecting" if project["captures"] else "empty")

    def state(self):
        with self.lock:
            projects = [{"id": p["id"], "name": p["name"], "status": p["status"],
                         "capture_count": len(p["captures"]), "updated_at": p["updated_at"]}
                        for p in self.projects.values()]
            projects.sort(key=lambda p: p["updated_at"], reverse=True)
            return {"app": APP_ID, "version": __version__, "projects": projects,
                    "load_errors": list(self.load_errors),
                    "capabilities": {"sensor": "Spike", "mode": "offline", "hardware_connected": False,
                                     "optics": "shared lens / 1:1 splitter / Spike branch", "max_upload_bytes": 64*1024**2,
                                     "inputs": ["png", "jpg", "tif", "npy", "dat (explicit layout)", "observations.json"]}}

    def create_project(self, body):
        name = body.get("name", "Spike 标定工程")
        if not isinstance(name, str) or not name.strip() or len(name.strip()) > 120:
            raise ValueError("工程名称应为 1～120 个字符")
        with self.lock:
            project = {"id": uuid.uuid4().hex, "name": name.strip(), "config": copy.deepcopy(DEFAULT_CONFIG),
                       "captures": [], "model": None, "validation": None, "turntable": None,
                       "revision": 0, "status": "empty", "updated_at": now()}
            return self._save(project)

    def get_project(self, project_id):
        with self.lock:
            return self._project(project_id)

    def update_config(self, project_id, body):
        with self.lock:
            project = self._project(project_id)
            config = copy.deepcopy(project["config"])
            patch = body.get("config", body)
            if not isinstance(patch, dict) or set(patch)-set(DEFAULT_CONFIG):
                raise ValueError("配置字段不正确")
            for key, value in patch.items():
                if isinstance(config.get(key), dict) and isinstance(value, dict):
                    config[key].update(value)
                else:
                    config[key] = value
            config = engine.validate_config(config)
            dumps(config)
            if config == project["config"]:
                return project
            geometry_changed = any(config[k] != project["config"][k] for k in ("target", "orientation", "optics"))
            project["config"] = config
            if geometry_changed:
                project["captures"] = []
                self._invalidate(project)
            else:
                project.update(validation=None, turntable=None,
                               status="solved" if project["model"] else "collecting" if project["captures"] else "empty")
            return self._save(project)

    def load_demo(self, project_id):
        from .fixtures import demo_captures
        with self.lock:
            project = self._project(project_id)
            if project["captures"]:
                raise ValueError("请新建工程载入演示，以保留现有实采观测")
            project["captures"] = demo_captures(project["config"])
            self._invalidate(project)
            return self._save(project)

    @staticmethod
    def _check_captures(captures, existing):
        import cv2
        import numpy as np
        if not isinstance(captures, list) or not captures or len(captures) > 100:
            raise ValueError("每批应导入 1～100 组观测")
        names = {c["name"] for c in existing}
        for c in captures:
            if not isinstance(c, dict):
                raise ValueError("观测必须为对象")
            c["id"] = uuid.uuid4().hex
            if not isinstance(c.get("name"), str) or not c["name"].strip() or c["name"] in names:
                raise ValueError("观测文件名为空或重复，请使用不同文件名")
            names.add(c["name"])
            if c.get("split") not in ("train", "test") or c.get("evidence") not in ("hardware", "synthetic", "public"):
                raise ValueError("需要明确训练/验证用途和数据来源")
            if not isinstance(c.get("sequence_id"), str) or not c["sequence_id"].strip():
                raise ValueError("请填写真实采集序列标识")
            if not isinstance(c.get("enabled", True), bool) or not isinstance(c.get("found", True), bool):
                raise ValueError("enabled/found 应为布尔值")
            c.setdefault("enabled", True)
            c.setdefault("found", True)
            size = c.get("image_size")
            if not isinstance(size, list) or len(size) != 2 or any(isinstance(x, bool) or not isinstance(x, int) or not 2 <= x <= 12000 for x in size) or size[0]*size[1] > 25_000_000:
                raise ValueError("image_size 应为有效 [宽,高]，上限 2500 万像素")
            obj, xy = np.asarray(c.get("object_points", []), dtype=float), np.asarray(c.get("image_points", []), dtype=float)
            if c["found"]:
                if xy.ndim != 2 or xy.shape[1] != 2 or len(xy) < 6 or obj.shape != (len(xy), 3):
                    raise ValueError("每组有效观测至少需要 6 对二维/三维特征点")
                if not np.isfinite(xy).all() or not np.isfinite(obj).all():
                    raise ValueError("特征点必须为有限数值")
                if np.any(xy < 0) or np.any(xy >= size):
                    raise ValueError("特征点超出图像范围")
                if np.linalg.matrix_rank(obj-obj.mean(axis=0)) < 2:
                    raise ValueError("标定点不能共线")
            c.setdefault("source", {})
            if not isinstance(c["source"], dict):
                raise ValueError("source 应为对象")
            if c.get("coordinate_domain", "transformed_pixels") != "transformed_pixels" or c["source"].get("coordinate_domain", "transformed_pixels") != "transformed_pixels":
                raise ValueError("观测坐标必须已应用当前旋转/镜像配置，使用 transformed_pixels")
            c["source"].setdefault("coordinate_domain", "transformed_pixels")
            normalized = engine.validate_capture(c)
            c.clear()
            c.update(normalized)
            c.setdefault("quality", {})
            preview = c.get("preview_base64", "")
            if not isinstance(preview, str) or len(preview) > 24*1024**2:
                raise ValueError("预览图过大或格式错误")
            if preview:
                try:
                    binary = base64.b64decode(preview, validate=True)
                except ValueError as exc:
                    raise ValueError("预览图必须为有效 base64") from exc
                image = cv2.imdecode(np.frombuffer(binary, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
                if image is None or [image.shape[1], image.shape[0]] != size:
                    raise ValueError("预览图无法解码或尺寸与 image_size 不一致")
        all_captures = existing + captures
        train_ids = {c["sequence_id"] for c in all_captures if c["enabled"] and c["split"] == "train"}
        test_ids = {c["sequence_id"] for c in all_captures if c["enabled"] and c["split"] == "test"}
        if train_ids & test_ids:
            raise ValueError("求解与独立验证不能使用同一采集序列，请按完整序列划分")
        if len(all_captures) > 300:
            raise ValueError("一个工程最多保留 300 组观测，请新建工程")

    def import_files(self, project_id, body):
        from .importers import import_capture
        files = body.get("files")
        if not isinstance(files, list) or not files or len(files) > 100:
            raise ValueError("请选择 1～100 个文件")
        with self.lock:
            project = self._project(project_id)
            captures = [import_capture(f, project["config"], split=body.get("split", "train"),
                                       sequence_id=body.get("sequence_id", ""), evidence=body.get("evidence", "hardware"),
                                       options=body.get("options", {})) for f in files]
            self._check_captures(captures, project["captures"])
            project["captures"].extend(captures)
            self._invalidate(project)
            return self._save(project)

    def import_observations(self, project_id, body):
        with self.lock:
            project = self._project(project_id)
            captures = copy.deepcopy(body.get("captures"))
            self._check_captures(captures, project["captures"])
            project["captures"].extend(captures)
            self._invalidate(project)
            return self._save(project)

    def update_capture(self, project_id, capture_id, body):
        if not isinstance(body, dict) or not body or set(body)-{"enabled", "split"}:
            raise ValueError("只允许修改启用状态和用途")
        with self.lock:
            project = self._project(project_id)
            capture = next((c for c in project["captures"] if c["id"] == capture_id), None)
            if capture is None:
                raise KeyError("观测不存在")
            if "enabled" in body and not isinstance(body["enabled"], bool):
                raise ValueError("enabled 应为布尔值")
            if "split" in body and body["split"] not in ("train", "test"):
                raise ValueError("用途应为 train 或 test")
            capture.update(body)
            if {c["sequence_id"] for c in project["captures"] if c["enabled"] and c["split"] == "train"} & {c["sequence_id"] for c in project["captures"] if c["enabled"] and c["split"] == "test"}:
                raise ValueError("同一采集序列不能同时用于求解和独立验证")
            self._invalidate(project)
            return self._save(project)

    def solve(self, project_id):
        with self.lock:
            project = self._project(project_id)
            model = engine.solve_intrinsics(project["captures"], project["config"])
            model["config_fingerprint"] = fingerprint(project["config"])
            project.update(model=model, validation=None, turntable=None, status="solved")
            return self._save(project)

    def validate(self, project_id):
        with self.lock:
            project = self._project(project_id)
            if project["model"] is None:
                raise ValueError("请先完成内参求解")
            project["validation"] = engine.validate_intrinsics(project["model"], project["captures"], project["config"])
            project["status"] = "validated"
            return self._save(project)

    def calibrate_turntable(self, project_id, body):
        with self.lock:
            project = self._project(project_id)
            if project["model"] is None:
                raise ValueError("请先完成内参求解")
            project["turntable"] = engine.fit_turntable(project["model"], body.get("observations", []), body.get("reference", {}), project["config"])
            return self._save(project)

    def query(self, project_id, body):
        with self.lock:
            project = self._project(project_id)
            if project["model"] is None:
                raise ValueError("请先完成内参求解")
            return engine.query_rays(project["model"], body.get("points", []), project["turntable"], body.get("encoder_deg"))

    def preview(self, project_id, capture_id, undistort=False):
        import cv2
        import numpy as np
        with self.lock:
            project = self._project(project_id)
        capture = next((c for c in project["captures"] if c["id"] == capture_id), None)
        if capture is None:
            raise KeyError("观测不存在")
        encoded = capture.get("preview_base64")
        image = cv2.imdecode(np.frombuffer(base64.b64decode(encoded), dtype=np.uint8), cv2.IMREAD_COLOR) if encoded else None
        if image is None:
            width, height = capture["image_size"]
            image = np.full((height, width, 3), 35, np.uint8)
        xy = np.asarray(capture["image_points"], dtype=np.float32)
        if undistort:
            if project["model"] is None:
                raise ValueError("请先完成内参求解")
            K, d = np.asarray(project["model"]["K"]), np.asarray(project["model"]["distortion"])
            image = cv2.undistort(image, K, d)
            if len(xy):
                xy = cv2.undistortPoints(xy.reshape(-1, 1, 2), K, d, P=K).reshape(-1, 2)
        for i, point in enumerate(xy):
            pt = tuple(np.round(point).astype(int))
            cv2.circle(image, pt, max(2, image.shape[1]//250), (50, 210, 255) if i else (70, 230, 70), 1, cv2.LINE_AA)
        success, binary = cv2.imencode(".png", image)
        if not success:
            raise ValueError("无法编码预览图")
        return binary.tobytes()

    def export_project(self, project_id):
        project = self.get_project(project_id)
        evidence = sorted({c["evidence"] for c in project["captures"] if c["enabled"]})
        return {"format": "spike-calibration-bundle", "format_version": 1, "software_version": __version__,
                "exported_at": now(), "sensor": "Spike", "optical_configuration": "shared_lens_1_to_1_splitter",
                "coordinate_convention": "pixels after configured rotation/flips; azimuth right-positive; elevation up-positive",
                "evidence": evidence, "hardware_certified": False,
                "limitations": ["标定仅绑定当前镜头、对焦、分光器安装及 Spike 图像坐标配置。",
                                "软件验证或合成结果不代表实机精度验收；阈值需依据实际要求填写。",
                                "单轴转台模型不估计完整六自由度；近距离模式依赖实测目标与旋转中心几何。"],
                "project": project}

    def report_html(self, project_id):
        bundle = self.export_project(project_id)
        p = bundle["project"]
        esc = lambda value: html.escape(str(value))
        sections = []
        for title, value in (("设备及标定配置", p["config"]), ("内参和畸变", p["model"]),
                             ("独立验证", p["validation"]), ("单轴转台", p["turntable"])):
            sections.append(f"<section><h2>{title}</h2><pre>{esc(dumps(value))}</pre></section>")
        rows = "".join(f"<tr><td>{esc(c['name'])}</td><td>{esc(c['sequence_id'])}</td><td>{esc(c['split'])}</td><td>{esc(c['evidence'])}</td><td>{'启用' if c['enabled'] else '排除'}</td><td>{len(c['image_points'])}</td></tr>" for c in p["captures"])
        limits = "".join(f"<li>{esc(x)}</li>" for x in bundle["limitations"])
        return f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>{esc(p['name'])} · Spike 标定报告</title>
<style>body{{max-width:1080px;margin:40px auto;padding:0 24px;font:15px/1.7 "Microsoft YaHei",sans-serif;color:#203047}}h1{{font-size:28px}}h2{{font-size:19px;margin-top:32px}}pre{{background:#f3f6fa;padding:18px;white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.6 Consolas,monospace}}table{{border-collapse:collapse;width:100%}}th,td{{padding:8px;text-align:left;border-bottom:1px solid #dae1e9}}.note{{border-left:3px solid #1668dc;background:#eef5ff;padding:14px}}@media print{{body{{margin:0}}pre{{font-size:10px}}h2{{break-after:avoid}}}}</style>
<h1>{esc(p['name'])}</h1><p>Spike 单支路标定报告 · 软件 {esc(__version__)} · {esc(bundle['exported_at'])}</p>
<p class="note">共用一个镜头，经 1:1 分光器进入 Spike 支路。离线软件结果；未认证实机合格。数据来源：{esc(', '.join(bundle['evidence']) or '无')}。</p>
{''.join(sections)}<section><h2>观测来源与划分</h2><table><thead><tr><th>文件</th><th>采集序列</th><th>用途</th><th>来源</th><th>状态</th><th>特征点</th></tr></thead><tbody>{rows}</tbody></table></section>
<h2>适用范围</h2><ul>{limits}</ul><p>工程 {esc(p['id'])} · 修订 {p['revision']}</p></html>'''
