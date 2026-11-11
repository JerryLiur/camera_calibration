"""Interpretable target observations and capture guidance for both sensor branches."""
from __future__ import annotations

import cv2
import numpy as np


def grayscale(image, *, black_level=0.0, white_level=255.0):
    data = np.asarray(image)
    if data.ndim not in (2, 3) or data.size == 0 or data.shape[0]*data.shape[1] > 25_000_000:
        raise ValueError("image must be a nonempty 2D or RGB image, up to 25 megapixels")
    if data.ndim == 3 and data.shape[2] not in (3, 4):
        raise ValueError("color image must have 3 or 4 channels")
    if not np.isfinite(data).all() or not np.isfinite([black_level, white_level]).all() or white_level <= black_level:
        raise ValueError("finite pixels and ordered black/white levels are required")
    gray = np.clip((data.astype(float)-black_level)/(white_level-black_level)*255, 0, 255).astype(np.uint8)
    if gray.ndim == 3:
        gray = cv2.cvtColor(gray, cv2.COLOR_RGBA2GRAY if gray.shape[2] == 4 else cv2.COLOR_RGB2GRAY)
    return gray


def image_quality(image, *, black_level=0.0, white_level=255.0, points=None):
    data = np.asarray(image, dtype=float)
    gray = grayscale(data, black_level=black_level, white_level=white_level)
    metrics = {"sharpness_laplacian_variance": float(cv2.Laplacian(gray, cv2.CV_64F).var()),
               "contrast_std": float(gray.std()/255), "mean_normalized": float(gray.mean()/255),
               "saturated_fraction": float(np.mean(data >= white_level)),
               "black_fraction": float(np.mean(data <= black_level)),
               "image_size": [int(gray.shape[1]), int(gray.shape[0])],
               "quality_thresholds": "not applied; configure them for the actual sensor/mode"}
    suggestions = []
    if metrics["contrast_std"] < 0.03:
        suggestions.append("低对比度：检查照明、阈值和重建窗长")
    if metrics["saturated_fraction"] > 0.05:
        suggestions.append("部分像素剪切：降低光源亮度或增益")
    if points is not None:
        metrics["feature_coverage"] = capture_guidance([points], metrics["image_size"])
    return {"metrics": metrics, "suggestions": suggestions,
            "heuristic_note": "建议采用可配置经验阈值，不是客户验收判定"}


def _target(target):
    kind = target.get("kind", "chessboard")
    cols, rows = target.get("cols", 9), target.get("rows", 6)
    if any(isinstance(x, bool) or not isinstance(x, int) or not 2 <= x <= 100 for x in (cols, rows)):
        raise ValueError("target rows/cols must be integers from 2 to 100")
    length = float(target.get("square_length_m", 0.025))
    if not np.isfinite(length) or length <= 0:
        raise ValueError("target spacing must be finite and positive")
    if kind not in ("chessboard", "charuco", "circles", "asymmetric_circles"):
        raise ValueError("unsupported calibration target")
    return kind, cols, rows, length


def detect_target(image, target):
    kind, cols, rows, length = _target(target)
    gray = grayscale(image, black_level=float(target.get("black_level", 0)),
                     white_level=float(target.get("white_level", 255)))
    ids, object_points = None, None
    if kind == "chessboard":
        found, corners = cv2.findChessboardCornersSB(gray, (cols, rows), flags=cv2.CALIB_CB_NORMALIZE_IMAGE)
        object_points = np.zeros((cols*rows, 3), dtype=float)
        object_points[:, :2] = np.mgrid[0:cols, 0:rows].T.reshape(-1, 2)*length
    elif kind == "charuco":
        if not hasattr(cv2, "aruco"):
            raise ValueError("ChArUco requires opencv-contrib")
        dictionary_name = target.get("dictionary", "DICT_4X4_50")
        if not isinstance(dictionary_name, str) or not dictionary_name.startswith("DICT_") or not hasattr(cv2.aruco, dictionary_name):
            raise ValueError("unknown ArUco dictionary")
        dictionary = cv2.aruco.getPredefinedDictionary(getattr(cv2.aruco, dictionary_name))
        marker_length = float(target.get("marker_length_m", length*0.7))
        if not 0 < marker_length < length:
            raise ValueError("marker length must be smaller than square length")
        board = cv2.aruco.CharucoBoard((cols, rows), length, marker_length, dictionary)
        corners, ids, _, _ = cv2.aruco.CharucoDetector(board).detectBoard(gray)
        found = corners is not None and len(corners) >= 4
        if found:
            ids = ids.reshape(-1)
            object_points = board.getChessboardCorners()[ids].astype(float)
    else:
        flags = cv2.CALIB_CB_ASYMMETRIC_GRID if kind == "asymmetric_circles" else cv2.CALIB_CB_SYMMETRIC_GRID
        params = cv2.SimpleBlobDetector_Params()
        params.minArea, params.maxArea = 8, max(100, gray.size/20)
        params.filterByArea = True
        detector = cv2.SimpleBlobDetector_create(params)
        try:
            found, corners = cv2.findCirclesGrid(gray, (cols, rows), flags=flags, blobDetector=detector)
        except cv2.error:
            found, corners = cv2.findCirclesGrid(gray, (cols, rows), flags, detector, cv2.CirclesGridFinderParameters())
        object_points = np.array([[(2*x+y % 2 if kind == "asymmetric_circles" else x)*length,
                                   y*length, 0] for y in range(rows) for x in range(cols)])
    if not found:
        return {"found": False, "points": [], "object_points": [], "ids": [],
                "target": target, "quality": image_quality(image),
                "reasons": ["target_not_detected; check target dimensions, contrast, focus and visibility"]}
    xy = corners.reshape(-1, 2).astype(float)
    return {"found": True, "points": xy.tolist(), "object_points": object_points.tolist(),
            "ids": ids.tolist() if ids is not None else list(range(len(xy))),
            "target": target, "quality": image_quality(image, points=xy),
            "localization": "OpenCV target localization; no calibrated point covariance available",
            "coordinate_domain": "raw_pixels", "manual_edits": []}


def capture_guidance(views, image_size, *, grid=(6, 4)):
    width, height = image_size
    if width <= 0 or height <= 0:
        raise ValueError("image dimensions must be positive")
    occupancy = np.zeros((grid[1], grid[0]), dtype=bool)
    centroids, areas, angles = [], [], []
    for points in views:
        xy = np.asarray(points, dtype=float)
        if xy.ndim != 2 or xy.shape[1] != 2 or len(xy) < 4 or not np.isfinite(xy).all():
            raise ValueError("each view must contain at least four finite points")
        inside = (xy[:, 0] >= 0) & (xy[:, 0] < width) & (xy[:, 1] >= 0) & (xy[:, 1] < height)
        valid = xy[inside]
        if len(valid):
            cells = np.floor(valid/[width, height]*grid).astype(int)
            occupancy[cells[:, 1], cells[:, 0]] = True
        centroids.append((xy.mean(axis=0)/[width, height]).tolist())
        hull = cv2.convexHull(xy.astype(np.float32))
        areas.append(float(cv2.contourArea(hull)/(width*height)))
        vector = xy[-1]-xy[0]
        angles.append(float(np.arctan2(vector[1], vector[0])))
    missing = [[x, y] for y in range(grid[1]) for x in range(grid[0]) if not occupancy[y, x]]
    recommendations = []
    if missing:
        recommendations.append("补采尚未覆盖的视场区域，特别是四角与边缘")
    if len(views) < 6:
        recommendations.append("内参求解至少补充至六组独立姿态，并改变倾角与距离")
    if len(views) > 1 and np.ptp(areas) < 0.05:
        recommendations.append("靶标成像尺寸变化较小，增加不同距离的观测")
    if len(views) > 1 and np.ptp(np.unwrap(angles)) < 0.2:
        recommendations.append("靶标旋转变化较小，增加不同倾角；二维指标不能代替三维姿态可观测性")
    return {"grid": list(grid), "occupied_cells": occupancy.astype(int).tolist(),
            "coverage_fraction": float(occupancy.mean()), "missing_cells": missing,
            "view_count": len(views), "centroids_normalized": centroids,
            "board_area_fractions": areas, "recommendations": recommendations}


def edit_observation(observation, edits, operator):
    import copy
    if not str(operator).strip():
        raise ValueError("manual feature edits require an operator")
    result = copy.deepcopy(observation)
    original_ids = result.get("ids", list(range(len(result["points"]))))
    mapped = {int(i): list(p) for i, p in zip(original_ids, result["points"])}
    objects = {int(i): list(p) for i, p in zip(original_ids, result.get("object_points", []))}
    for edit in edits:
        corner_id = int(edit["id"])
        if edit["action"] == "delete":
            if corner_id not in mapped:
                raise ValueError("cannot delete an unknown corner")
            del mapped[corner_id]
            objects.pop(corner_id, None)
        elif edit["action"] in ("set", "add"):
            point = np.asarray(edit["point"], dtype=float)
            if point.shape != (2,) or not np.isfinite(point).all():
                raise ValueError("edited point must contain two finite coordinates")
            if edit["action"] == "add" and corner_id in mapped:
                raise ValueError("corner id already exists")
            if corner_id not in mapped and "object_point" not in edit:
                raise ValueError("adding a corner requires its target object_point")
            mapped[corner_id] = point.tolist()
            if "object_point" in edit:
                obj = np.asarray(edit["object_point"], dtype=float)
                if obj.shape != (3,) or not np.isfinite(obj).all():
                    raise ValueError("object_point must be finite [x,y,z]")
                objects[corner_id] = obj.tolist()
        else:
            raise ValueError("unknown edit action")
    result["ids"] = sorted(mapped)
    result["points"] = [mapped[i] for i in result["ids"]]
    result["object_points"] = [objects[i] for i in result["ids"] if i in objects]
    result.setdefault("manual_edits", []).append({"operator": operator, "edits": edits})
    return result


def spike_representation(data, method="count", period_s=None):
    values = np.asarray(data)
    if values.ndim != 3 or not len(values) or not np.all((values == 0) | (values == 1)):
        raise ValueError("raw Spike must be a nonempty binary [T,H,W] array")
    if period_s is not None and (not np.isfinite(period_s) or period_s <= 0):
        raise ValueError("sample period must be finite and positive")
    if method == "count":
        result = values.sum(axis=0, dtype=np.uint64)
        unit = "spikes_per_window"
    elif method == "rate":
        if period_s is None:
            raise ValueError("count rate requires a verified sample period")
        result = values.sum(axis=0)/(len(values)*period_s)
        unit = "Hz"
    elif method == "interval":
        indices = np.arange(len(values))[:, None, None]
        first = np.min(np.where(values, indices, len(values)), axis=0)
        last = np.max(np.where(values, indices, -1), axis=0)
        counts = values.sum(axis=0)
        valid = counts >= 2
        duration = np.maximum(last-first, 1)
        result = np.where(valid, (counts-1)/duration, 0)
        if period_s is not None:
            result = result/period_s
        unit = "inverse_ticks" if period_s is None else "Hz"
    else:
        raise ValueError("representation must be count, rate or interval")
    active = values.sum(axis=0)
    valid = active >= 2 if method == "interval" else np.ones(active.shape, dtype=bool)
    return {"image": result, "valid_mask": valid, "method": method, "unit": unit,
            "window_ticks": [0, len(values)], "period_s": period_s,
            "observation_semantics": "window statistic, not an instantaneous image",
            "quality": {"all_zero_fraction": float((active == 0).mean()),
                        "always_firing_fraction": float((active == len(values)).mean())}}
