"""Single-branch Spike geometry; no RGB parameters or raw-spike resampling."""
from __future__ import annotations

import copy
import hashlib
import math

import cv2
import numpy as np

from calibration_system.targets import capture_guidance
from rgb_spike_fusion.spatial_calibration import fit_camera_intrinsics


DEFAULT_CONFIG = {
    "target": {"kind": "chessboard", "cols": 9, "rows": 6, "square_length_m": 0.025},
    "orientation": {"rotation_deg": 0, "flip_x": False, "flip_y": False},
    "optics": {"shared_lens": True, "split_ratio": "1:1", "lens_id": "", "focus_note": "", "splitter_note": ""},
    "acceptance": {"intrinsics_p95_px": None, "turntable_p95_deg": None},
    "operator": "",
}


def _finite(value, name, shape=None):
    try:
        result = np.asarray(value, dtype=float)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} 必须为有限数值") from exc
    if not np.isfinite(result).all() or (shape is not None and result.shape != shape):
        raise ValueError(f"{name} 的形状或数值无效")
    return result


def _number(value, name, positive=False):
    if isinstance(value, bool):
        raise ValueError(f"{name} 必须为数值")
    number = float(value)
    if not math.isfinite(number) or (positive and number <= 0):
        raise ValueError(f"{name} 必须为有限{'正' if positive else ''}数")
    return number


def validate_config(config):
    if not isinstance(config, dict):
        raise ValueError("配置必须为对象")
    result = copy.deepcopy(DEFAULT_CONFIG)
    for key, value in config.items():
        if key not in result:
            raise ValueError(f"未知配置字段：{key}")
        if isinstance(result[key], dict):
            if not isinstance(value, dict):
                raise ValueError(f"{key} 必须为对象")
            unknown = set(value) - set(result[key])
            if unknown:
                raise ValueError(f"未知配置字段：{key}.{sorted(unknown)[0]}")
            result[key].update(value)
        else:
            result[key] = value
    target = result["target"]
    if target["kind"] not in ("chessboard", "circles", "asymmetric_circles"):
        raise ValueError("靶标支持棋盘格、圆点或非对称圆点阵列")
    for key in ("cols", "rows"):
        n = target[key]
        if isinstance(n, bool) or not isinstance(n, int) or not 2 <= n <= 100:
            raise ValueError("靶标行列必须为 2～100 的整数（棋盘格填写内角点数）")
    target["square_length_m"] = _number(target["square_length_m"], "靶标间距", True)
    ori = result["orientation"]
    if type(ori["rotation_deg"]) is not int or ori["rotation_deg"] not in (0, 90, 180, 270):
        raise ValueError("图像旋转角必须为 0、90、180 或 270 度（顺时针）")
    if any(type(ori[k]) is not bool for k in ("flip_x", "flip_y")):
        raise ValueError("图像镜像配置必须为布尔值")
    if result["optics"]["shared_lens"] is not True or result["optics"]["split_ratio"] != "1:1":
        raise ValueError("本工具对应共用一个镜头、1:1 分光的完整 Spike 支路")
    for key in ("lens_id", "focus_note", "splitter_note"):
        if not isinstance(result["optics"][key], str) or len(result["optics"][key]) > 1000:
            raise ValueError(f"{key} 必须为不超过 1000 字的文本")
    for key, value in result["acceptance"].items():
        if value is not None:
            result["acceptance"][key] = _number(value, key, True)
    if not isinstance(result["operator"], str) or len(result["operator"]) > 200:
        raise ValueError("操作人必须为不超过 200 字的文本")
    return result


def validate_capture(capture):
    """Check the shared observation schema also used by direct JSON imports."""
    if not isinstance(capture, dict):
        raise ValueError("每条采集观测必须为对象")
    result = copy.deepcopy(capture)
    for key in ("id", "name", "sequence_id"):
        if not isinstance(result.get(key), str) or not result[key].strip() or len(result[key]) > 500:
            raise ValueError(f"采集 {key} 必须为有效文本")
    if result.get("split") not in ("train", "test"):
        raise ValueError("观测 split 必须为 train 或 test")
    if result.get("evidence") not in ("synthetic", "hardware", "public"):
        raise ValueError("观测必须明确标记来源 synthetic、hardware 或 public")
    if type(result.get("enabled", True)) is not bool or type(result.get("found", True)) is not bool:
        raise ValueError("enabled/found 必须为布尔值")
    result.setdefault("enabled", True)
    result.setdefault("found", True)
    size = result.get("image_size")
    if not isinstance(size, (list, tuple)) or len(size) != 2 or any(type(n) is not int or n <= 0 for n in size) or size[0]*size[1] > 25_000_000:
        raise ValueError("图像尺寸必须为有效的 [宽,高]，且不超过 2500 万像素")
    if result["found"]:
        image = _finite(result.get("image_points"), "像点")
        obj = _finite(result.get("object_points"), "靶标三维点")
        if image.ndim != 2 or image.shape[1] != 2 or not 6 <= len(image) <= 10000 or obj.shape != (len(image), 3):
            raise ValueError("观测需要至少六个匹配的 [u,v] 像点与 [X,Y,Z] 靶标点")
        if not np.allclose(obj[:, 2], 0, atol=1e-10) or np.linalg.matrix_rank(obj[:, :2]-obj[:, :2].mean(axis=0)) < 2:
            raise ValueError("靶标点必须为 Z=0 的非共线平面点，单位为米")
        if np.any(image < 0) or np.any(image >= np.asarray(size)):
            raise ValueError("角点超出图像范围")
        if len(np.unique(image, axis=0)) != len(image) or len(np.unique(obj, axis=0)) != len(obj):
            raise ValueError("角点与靶标点不得重复")
        result["image_points"], result["object_points"] = image.tolist(), obj.tolist()
    elif result.get("image_points") or result.get("object_points"):
        raise ValueError("found=false 的观测不得携带有效角点")
    result.setdefault("coordinate_domain", "transformed_pixels")
    if result["coordinate_domain"] != "transformed_pixels":
        raise ValueError("导入角点必须已经按工程的旋转/镜像设置转换到 transformed_pixels")
    result.setdefault("quality", {})
    result.setdefault("source", {})
    if not isinstance(result["quality"], dict) or not isinstance(result["source"], dict):
        raise ValueError("quality/source 必须为对象")
    digest = result["source"].get("sha256")
    if digest is not None and (not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdefABCDEF" for c in digest)):
        raise ValueError("source.sha256 必须为64位十六进制文本")
    return result


def _active(captures):
    checked = [validate_capture(c) for c in captures if c.get("enabled", True) and c.get("found", True)]
    train_seq = {c["sequence_id"] for c in checked if c["split"] == "train"}
    test_seq = {c["sequence_id"] for c in checked if c["split"] == "test"}
    if train_seq & test_seq:
        raise ValueError("训练与独立验证不得使用同一采集序列，请重新划分 sequence_id")
    if len({c["id"] for c in checked}) != len(checked):
        raise ValueError("采集 ID 不得重复")
    train_signatures = {_signature(c) for c in checked if c["split"] == "train"}
    if any(_signature(c) in train_signatures for c in checked if c["split"] == "test"):
        raise ValueError("训练与验证包含完全相同的角点观测，修改文件名或序列名不能构成独立验证")
    return checked


def _signature(capture):
    binary = np.asarray(capture["image_points"], dtype="<f8").tobytes()+np.asarray(capture["object_points"], dtype="<f8").tobytes()
    return hashlib.sha256(binary).hexdigest()


def _stats(errors, unit="px"):
    a = _finite(errors, "残差").reshape(-1)
    if not len(a):
        raise ValueError("缺少残差样本")
    return {"count": int(len(a)), f"rms_{unit}": float(np.sqrt(np.mean(a*a))),
            f"mean_{unit}": float(np.mean(a)), f"p95_{unit}": float(np.percentile(a, 95)),
            f"max_{unit}": float(np.max(a))}


def _camera(model):
    if not isinstance(model, dict):
        raise ValueError("请先求解 Spike 内参")
    K = _finite(model.get("K"), "内参 K", (3, 3))
    d = _finite(model.get("distortion"), "畸变参数").reshape(-1)
    size = model.get("image_size")
    if K[0, 0] <= 0 or K[1, 1] <= 0 or not np.allclose(K[2], [0, 0, 1]) or len(d) not in (4, 5, 8, 12, 14):
        raise ValueError("相机内参或畸变参数无效")
    if not isinstance(size, (list, tuple)) or len(size) != 2 or any(type(n) is not int or n <= 0 for n in size):
        raise ValueError("标定模型缺少有效图像尺寸")
    return K, d, tuple(size)


def solve_intrinsics(captures, config):
    config = validate_config(config)
    active = _active(captures)
    views = [c for c in active if c["split"] == "train"]
    if len(views) < 6:
        raise ValueError("内参求解至少需要六个有效训练姿态，请改变标定板倾角、位置和距离")
    size = tuple(views[0]["image_size"])
    if any(tuple(c["image_size"]) != size for c in active):
        raise ValueError("同一工程的有效观测必须使用相同图像尺寸")
    signatures = {np.asarray(c["image_points"], dtype=np.float64).tobytes() for c in views}
    if len(signatures) < 6:
        raise ValueError("训练姿态重复，至少需要六个不同姿态")
    constraints = []
    for c in views:
        obj = np.asarray(c["object_points"], dtype=float)
        xy = np.asarray(c["image_points"], dtype=float)
        H, _ = cv2.findHomography(obj[:, :2], (xy-np.asarray(size)/2)/max(size), 0)
        if H is None:
            raise ValueError("靶标投影退化，请补采不同倾角")
        def v(i, j):
            return np.array([H[0,i]*H[0,j], H[0,i]*H[1,j]+H[1,i]*H[0,j], H[1,i]*H[1,j],
                             H[2,i]*H[0,j]+H[0,i]*H[2,j], H[2,i]*H[1,j]+H[1,i]*H[2,j], H[2,i]*H[2,j]])
        for row in (v(0,1), v(0,0)-v(1,1)):
            constraints.append(row/max(np.linalg.norm(row), 1e-15))
    singular = np.linalg.svd(constraints, compute_uv=False)
    if singular[-2]/singular[0] < 1e-4:
        raise ValueError("姿态约束退化：只平移或正对相机不足以标定，请增加不同倾角")
    fitted = fit_camera_intrinsics([c["object_points"] for c in views], [c["image_points"] for c in views], size,
                                   provenance="assembled shared-lens splitter Spike branch")
    K, d = fitted.camera.K, fitted.camera.distortion
    if not np.isfinite(K).all() or not np.isfinite(d).all() or not math.isfinite(fitted.rms_px):
        raise ValueError("内参求解产生无效参数，请检查角点、板尺寸及姿态")
    coverage = capture_guidance([c["image_points"] for c in views], size)
    warnings = list(coverage["recommendations"])
    if not 0 <= K[0,2] < size[0] or not 0 <= K[1,2] < size[1]:
        warnings.append("主点位于图像范围外，建议检查姿态分布及角点后重采")
    evidence = sorted({c["evidence"] for c in views})
    if evidence != ["hardware"]:
        warnings.append("包含合成或公开数据；结果不能作为本设备实机精度验收")
    return {"sensor": "spike", "model_type": "pinhole_brown_5", "K": K.tolist(), "distortion": d.reshape(-1).tolist(),
            "image_size": list(size), "coordinate_domain": "transformed_pixels", "orientation": config["orientation"],
            "evidence": evidence, "train_sequence_ids": sorted({c["sequence_id"] for c in views}),
            "train_capture_ids": [c["id"] for c in views], "train_source_hashes": sorted({c["source"].get("sha256") for c in views if c["source"].get("sha256")}),
            "train_observation_signatures": sorted({_signature(c) for c in views}),
            "diagnostics": {"fit_rms_px": float(fitted.rms_px), "per_view_rms_px": fitted.per_view_rms_px.tolist(),
                "per_view": [{"capture_id": c["id"], "name": c["name"], "rms_px": float(e)} for c, e in zip(views, fitted.per_view_rms_px)],
                "intrinsic_std": fitted.intrinsic_std.tolist() if fitted.intrinsic_std is not None else None,
                "constraint_singular_values": singular.tolist(), "coverage": coverage},
            "warnings": warnings, "scope": "完整镜头＋1:1分光器＋Spike支路；不包含RGB配准及双机时序"}


def validate_intrinsics(model, captures, config):
    config = validate_config(config)
    K, d, size = _camera(model)
    active = _active(captures)
    views = [c for c in active if c["split"] == "test"]
    if not views:
        raise ValueError("请导入与训练采集序列独立的验证姿态")
    train_seq = set(model.get("train_sequence_ids", []))
    train_ids = set(model.get("train_capture_ids", []))
    train_hashes = set(model.get("train_source_hashes", []))
    per_view, errors = [], []
    for c in views:
        if c["sequence_id"] in train_seq or c["id"] in train_ids or c["source"].get("sha256") in train_hashes or _signature(c) in model.get("train_observation_signatures", []):
            raise ValueError("验证数据与冻结模型的训练数据重叠，必须重新独立采集")
        if tuple(c["image_size"]) != size:
            raise ValueError("验证图像尺寸与冻结模型不同")
        obj, xy = np.asarray(c["object_points"], dtype=float), np.asarray(c["image_points"], dtype=float)
        ok, r, t = cv2.solvePnP(obj, xy, K, d)
        if not ok:
            raise ValueError("独立验证姿态估计失败")
        prediction = cv2.projectPoints(obj, r, t, K, d)[0].reshape(-1, 2)
        residual = np.linalg.norm(prediction-xy, axis=1)
        errors.extend(residual.tolist())
        per_view.append({"capture_id": c["id"], "sequence_id": c["sequence_id"], "name": c["name"], "stats": _stats(residual)})
    stats = _stats(errors)
    gate = config["acceptance"]["intrinsics_p95_px"]
    worst = max(v["stats"]["p95_px"] for v in per_view)
    status = "measured" if gate is None else ("passed" if worst <= gate else "failed")
    return {"status": status, "stats": stats, "p95_px": stats["p95_px"], "worst_view_p95_px": worst,
            "threshold_px": gate, "per_view": per_view, "test_sequence_ids": sorted({c["sequence_id"] for c in views}),
            "evidence": sorted({c["evidence"] for c in views}), "evidence_all_hardware": model.get("evidence") == ["hardware"] and all(c["evidence"] == "hardware" for c in views),
            "hardware_qualified": False,
            "note": "冻结K/畸变，仅拟合每个验证板的位姿；重投影误差不等同绝对测量精度。未设置阈值时仅报告测量结果。"}


def _wrap(degrees):
    return (np.asarray(degrees)+180.) % 360.-180.


def _reference(reference):
    if not isinstance(reference, dict):
        raise ValueError("转台参考必须为对象")
    result = copy.deepcopy(reference)
    if type(result.get("sign", 1)) is not int or result.get("sign", 1) not in (-1, 1):
        raise ValueError("转台正方向 sign 必须为 1 或 -1")
    result.setdefault("sign", 1)
    if result.get("mode") == "far_field":
        if result.get("far_field_confirmed") is not True:
            raise ValueError("远场近似必须明确确认；近距离目标请使用已知几何模式")
        result["reference_angle_deg"] = _number(result.get("reference_angle_deg", 0), "参考方向")
    elif result.get("mode") == "finite_distance":
        result["target_position_m"] = _finite(result.get("target_position_m"), "目标相对转台轴位置", (3,)).tolist()
        result["camera_offset_m"] = _finite(result.get("camera_offset_m"), "相机入口瞳相对转轴位置", (3,)).tolist()
        if np.linalg.norm(np.asarray(result["target_position_m"])-result["camera_offset_m"]) <= 1e-8:
            raise ValueError("目标不能位于相机入口瞳位置")
    else:
        raise ValueError("转台参考 mode 必须为 far_field 或 finite_distance")
    return result


def _expected_body_azimuth(reference, encoder):
    q = reference["sign"]*np.asarray(encoder, dtype=float)
    if reference["mode"] == "far_field":
        return _wrap(reference["reference_angle_deg"]-q)
    target, offset = np.asarray(reference["target_position_m"]), np.asarray(reference["camera_offset_m"])
    rad = np.radians(q)
    x = np.cos(rad)*target[0]-np.sin(rad)*target[2]-offset[0]
    z = np.sin(rad)*target[0]+np.cos(rad)*target[2]-offset[2]
    if np.any(z <= 0):
        raise ValueError("已知几何在部分转角下位于相机支路背面；请检查坐标及范围")
    return np.degrees(np.arctan2(x, z))


def fit_turntable(model, observations, reference, config):
    config, reference = validate_config(config), _reference(reference)
    if not isinstance(observations, list) or len(observations) < 4:
        raise ValueError("转台至少需要三条训练观测和一条独立验证观测")
    checked = []
    for observation in observations:
        if not isinstance(observation, dict) or observation.get("split") not in ("train", "test") or not isinstance(observation.get("sequence_id"), str) or not observation["sequence_id"].strip():
            raise ValueError("转台观测需要 split=train/test 与有效 sequence_id")
        q = _number(observation.get("encoder_deg"), "转台角度")
        pixel = _finite(observation.get("pixel"), "转台特征像点", (2,)).tolist()
        measured = query_rays(model, [pixel])["points"][0]["azimuth_deg"]
        checked.append({**observation, "encoder_deg": q, "pixel": pixel, "observed_azimuth_deg": measured})
    train = [o for o in checked if o["split"] == "train"]
    test = [o for o in checked if o["split"] == "test"]
    if len(train) < 3 or not test or np.ptp([o["encoder_deg"] for o in train]) < 1.0:
        raise ValueError("转台训练需至少三个观测且角度跨度至少1度，并保留独立验证")
    if {o["sequence_id"] for o in train} & {o["sequence_id"] for o in test}:
        raise ValueError("转台训练与验证必须使用独立采集序列")
    train_expected = _expected_body_azimuth(reference, [o["encoder_deg"] for o in train])
    offsets = _wrap(train_expected-[o["observed_azimuth_deg"] for o in train])
    sine, cosine = np.mean(np.sin(np.radians(offsets))), np.mean(np.cos(np.radians(offsets)))
    if np.hypot(sine, cosine) < 0.5:
        raise ValueError("零位关系不一致，请检查转台正方向、标记点和单位")
    zero = float(np.degrees(np.arctan2(sine, cosine)))
    for o, expected in zip(checked, _expected_body_azimuth(reference, [o["encoder_deg"] for o in checked])):
        o["predicted_azimuth_deg"] = float(_wrap(expected-zero))
        o["residual_deg"] = float(abs(_wrap(o["observed_azimuth_deg"]-o["predicted_azimuth_deg"])))
    train_stats = _stats([o["residual_deg"] for o in checked if o["split"] == "train"], "deg")
    test_stats = _stats([o["residual_deg"] for o in checked if o["split"] == "test"], "deg")
    gate = config["acceptance"]["turntable_p95_deg"]
    status = "measured" if gate is None else ("passed" if test_stats["p95_deg"] <= gate else "failed")
    return {"model_type": "single_axis_yaw_zero", "zero_offset_deg": zero, "sign": reference["sign"],
            "reference": reference, "train_stats": train_stats, "test_stats": test_stats, "test_p95_deg": test_stats["p95_deg"],
            "status": status, "threshold_deg": gate, "observations": checked,
            "evidence": model.get("evidence", []), "hardware_qualified": False,
            "encoder_range_deg": [float(min(o["encoder_deg"] for o in train)), float(max(o["encoder_deg"] for o in train))],
            "coordinate_convention": "零位时转台坐标X右、Y下、Z前；绕+Y旋转将Z指向+X；相机偏置为转动体坐标米。零位为相机相对该体的水平偏航。",
            "limitations": ["仅估计一个转轴的水平零位；假定俯仰/滚转已机械对准，不估计完整六自由度或机构回差模型。", "须使用停稳后的观测；动态编码器与Spike时间对齐另行验证。", "远场结果仅在已确认近似条件下适用；近距离模式依赖所填目标位置及入口瞳偏置的测量质量。"]}


def query_rays(model, points, turntable=None, encoder_deg=None):
    K, d, size = _camera(model)
    xy = _finite(points, "查询像点")
    if xy.ndim != 2 or xy.shape[1] != 2 or not 1 <= len(xy) <= 10000:
        raise ValueError("查询像点必须为非空 [[u,v],...]，最多10000点")
    if np.any(xy < 0) or np.any(xy >= np.asarray(size)):
        raise ValueError("查询像点超出标定图像范围")
    und = cv2.undistortPoints(xy[:, None, :], K, d).reshape(-1, 2)
    rays = np.column_stack((und, np.ones(len(und))))
    rays /= np.linalg.norm(rays, axis=1)[:, None]
    q = _number(encoder_deg, "当前转台角度") if encoder_deg is not None else None
    result = []
    for pixel, ray in zip(xy, rays):
        az = float(np.degrees(np.arctan2(ray[0], ray[2])))
        el = float(np.degrees(np.arctan2(-ray[1], np.hypot(ray[0], ray[2]))))
        record = {"pixel": pixel.tolist(), "ray_camera": ray.tolist(), "azimuth_deg": az, "elevation_deg": el}
        if turntable is not None:
            offset = _number(turntable.get("zero_offset_deg"), "转台零位")
            sign = turntable.get("sign")
            if sign not in (-1, 1):
                raise ValueError("转台模型方向无效")
            body = float(_wrap(az+offset))
            record["body_azimuth_deg"] = body
            if q is not None:
                record["world_azimuth_deg"] = float(_wrap(sign*q+body))
                if turntable.get("reference", {}).get("mode") == "far_field":
                    record["encoder_target_deg"] = float(q+az/sign)
                    record["encoder_target_semantics"] = "远场近似下按当前观测视线进行偏航补偿；仅适用于已确认的远场条件"
                else:
                    record["direction_only_note"] = "输出视线方向；未测目标深度，不计算有限距离目标的转台指向角"
                limits = turntable.get("encoder_range_deg")
                if limits:
                    record["encoder_within_calibrated_range"] = bool(limits[0] <= q <= limits[1])
        result.append(record)
    return {"points": result, "coordinate_domain": "transformed_pixels", "angle_convention": "水平向右为正，垂直向上为正；ray_camera按X右Y下Z前", "image_size": list(size)}
