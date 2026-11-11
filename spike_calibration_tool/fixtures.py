"""Deterministic multi-pose examples, visibly and structurally synthetic."""
from __future__ import annotations

import cv2
import numpy as np

from calibration_system.targets import image_quality
from .engine import validate_config, validate_capture
from .importers import apply_orientation, orient_points, encode_preview


DEMO_K = np.array([[620., 0., 319.], [0., 610., 239.], [0., 0., 1.]])
DEMO_DISTORTION = np.array([-0.055, 0.012, 0.0007, -0.0005, 0.])


def demo_captures(config):
    config = validate_config(config)
    rng = np.random.default_rng(20261104)
    target = config["target"]
    cols, rows, spacing = target["cols"], target["rows"], target["square_length_m"]
    if target["kind"] != "chessboard":
        raise ValueError("内置演示采用棋盘格，请先将靶标类型设置为 chessboard")
    obj = np.zeros((cols*rows, 3), dtype=float)
    obj[:, :2] = np.mgrid[0:cols, 0:rows].T.reshape(-1, 2)*spacing
    extent = max((cols+1)*spacing, (rows+1)*spacing)
    captures = []
    for i in range(16):
        rotation = np.array([(-1 if i % 2 else 1)*(0.16+0.055*(i % 5)), (-1 if i % 3 else 1)*(0.13+0.06*(i % 4)), (i % 5-2)*0.09])
        depth = extent*(1.55+0.18*(i % 4))
        translation = np.array([-(cols-1)*spacing/2+(i % 4-1.5)*extent*0.26,
                                -(rows-1)*spacing/2+(i % 3-1)*extent*0.18, depth])
        image_points = cv2.projectPoints(obj, rotation, translation, DEMO_K, DEMO_DISTORTION)[0].reshape(-1, 2)
        # Keep known corners on sensor even for unusual configured board aspect ratios.
        for _ in range(10):
            if np.all(image_points >= [8,8]) and np.all(image_points <= [631,471]):
                break
            translation[2] *= 1.09
            image_points = cv2.projectPoints(obj, rotation, translation, DEMO_K, DEMO_DISTORTION)[0].reshape(-1, 2)
        image_points += rng.normal(0., 0.035, image_points.shape)
        image = np.full((480, 640), 175, dtype=np.uint8)
        for y in range(-1, rows):
            for x in range(-1, cols):
                square = np.array([[x,y,0],[x+1,y,0],[x+1,y+1,0],[x,y+1,0]], dtype=float)*spacing
                polygon = cv2.projectPoints(square, rotation, translation, DEMO_K, DEMO_DISTORTION)[0].reshape(-1, 2)
                cv2.fillConvexPoly(image, np.rint(polygon).astype(np.int32), 235 if (x+y) % 2 else 25, lineType=cv2.LINE_AA)
        cv2.putText(image, "SYNTHETIC / GEOMETRY DEMO", (12, 21), cv2.FONT_HERSHEY_SIMPLEX, .44, 55, 1, cv2.LINE_AA)
        image = apply_orientation(image, config["orientation"])
        image_points, size = orient_points(image_points, [640,480], config["orientation"])
        split = "train" if i < 12 else "test"
        capture = {"id": f"demo-{i:02d}", "name": f"synthetic_pose_{i+1:02d}.png", "sequence_id": f"synthetic-{split}-{i:02d}",
            "split": split, "evidence": "synthetic", "enabled": True, "found": True, "image_size": size,
            "object_points": obj.tolist(), "image_points": image_points.tolist(), "coordinate_domain": "transformed_pixels",
            "quality": image_quality(image, points=image_points), "preview_base64": encode_preview(image),
            "source": {"kind": "synthetic_projected_board", "seed": 20261104, "orientation": config["orientation"],
                "note": "独立数值多姿态夹具，角点含0.035px合成扰动；不是实际相机或实际工作距离"}}
        captures.append(validate_capture(capture))
    return captures
