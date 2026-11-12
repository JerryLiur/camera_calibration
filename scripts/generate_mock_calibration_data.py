#!/usr/bin/env python
"""Generate independent, ground-truthed mock data for the Spike calibration tool.

The generator does NOT import any project module: it writes observations the same
way an external capture rig would. A separate JSON file records the exact camera
matrix, distortion coefficients and board poses so a checker can measure how
far the software's answer drifts from a known truth.

Scenarios
---------
clean       24 train + 8 test poses, 0.04 px corner noise  -> accuracy baseline
noisy       same poses, 0.35 px corner noise               -> robustness / gates
prism       clean plus a distortion the 5-parameter model cannot express
            -> "small reprojection error, wrong pointing angle" demonstration
degenerate  front-facing boards, no distortion, no noise   -> must be rejected
degenerate_noisy  same exact rank deficit + 0.04 px noise -> guard blind spot
flat_distorted  front-facing boards WITH radial distortion -> NOT rejected, but biased
leak        test set reuses train corners under new names   -> must be rejected
png         12 rendered chessboard images                   -> full image pipeline
spike_npy   3 binary [T,H,W] Spike streams                  -> raw-spike pipeline
packed_dat  1 bit-packed DAT (plus an over-limit window)    -> explicit layout

Usage
-----
    python scripts/generate_mock_calibration_data.py --output output/mock_calibration_data
    python scripts/generate_mock_calibration_data.py --help
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

# Ground truth deliberately differs from the tool's built-in demo (620/610, 25 mm).
WIDTH, HEIGHT = 640, 480
K_TRUE = np.array([[812.4, 0.0, 327.6],
                   [0.0, 805.9, 238.3],
                   [0.0, 0.0, 1.0]])
D_TRUE = np.array([-0.1432, 0.0871, 0.00113, -0.00076, 0.0214])
COLS, ROWS, SQUARE_M = 9, 6, 0.030
SAMPLE_RATE_HZ = 20_000
TURNTABLE_ZERO_DEG = -2.4
SEED = 20261112

SPIKE_TICKS_NPY = 128          # 6.4 ms @ 20 kHz; keeps NPY under the 64 MiB upload cap
SPIKE_TICKS_DAT = 384          # 19.2 ms @ 20 kHz, bit-packed -> ~14.7 MiB
FRAME_STRIDE_BYTES = (WIDTH * HEIGHT + 7) // 8

OBJ = np.zeros((COLS * ROWS, 3), dtype=float)
OBJ[:, :2] = np.mgrid[0:COLS, 0:ROWS].T.reshape(-1, 2) * SQUARE_M
BOARD_CENTER = np.array([OBJ[:, 0].mean(), OBJ[:, 1].mean(), 0.0])
TARGET = {"kind": "chessboard", "cols": COLS, "rows": ROWS, "square_length_m": SQUARE_M}

QUERY_PIXELS = [[327.6, 238.3], [100.0, 80.0], [560.0, 400.0],
                [327.6, 60.0], [60.0, 430.0], [610.0, 240.0]]
TURNTABLE_TRAIN = [-9.0, -5.0, 0.0, 4.0, 9.0]
TURNTABLE_TEST = [-7.0, 2.0, 7.0]
FINITE_TARGET = np.array([0.30, 0.04, 1.75])
FINITE_OFFSET = np.array([0.12, 0.01, 0.06])


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")


def project(obj, rvec, tvec, distortion=None):
    return cv2.projectPoints(obj, rvec, tvec, K_TRUE,
                             D_TRUE if distortion is None else distortion)[0].reshape(-1, 2)


def make_pose(rng: np.random.Generator, index: int, *, front_facing: bool = False,
              distortion=None) -> dict:
    """Place the board at one of 12 field positions with a genuinely tilted pose."""
    gx, gy = index % 4, (index // 4) % 3
    cu, cv = WIDTH * (0.26 + 0.16 * gx), HEIGHT * (0.24 + 0.26 * gy)
    depth = (0.62 + 0.035 * index) if front_facing else (
        0.62 + 0.16 * (index % 4) + 0.06 * ((index // 12) % 3))
    if front_facing:
        rvec = np.zeros(3)
    else:
        rvec = np.array([rng.uniform(-0.34, 0.34), rng.uniform(-0.32, 0.32), rng.uniform(-0.45, 0.45)])
        rvec[0] = math.copysign(max(abs(rvec[0]), 0.11), rvec[0] or 1.0)
        rvec[1] = math.copysign(max(abs(rvec[1]), 0.10), rvec[1] or -1.0)
    for attempt in range(80):
        rotation = cv2.Rodrigues(rvec)[0]
        camera_point = np.array([(cu - K_TRUE[0, 2]) / K_TRUE[0, 0] * depth,
                                 (cv - K_TRUE[1, 2]) / K_TRUE[1, 1] * depth, depth])
        tvec = camera_point - rotation @ BOARD_CENTER
        image_points = project(OBJ, rvec, tvec, distortion)
        if np.all(image_points >= [16, 16]) and np.all(image_points <= [WIDTH - 16, HEIGHT - 16]):
            return {"index": index, "rvec": rvec.tolist(), "tvec": tvec.tolist(),
                    "depth_m": float(depth), "board_center_px": [float(cu), float(cv)],
                    "image_points_truth": image_points.tolist()}
        if attempt and attempt % 16 == 15:
            rvec *= 0.85
        depth *= 1.04
    raise RuntimeError(f"pose {index}: could not keep all corners inside the sensor")


def render_board(pose: dict) -> np.ndarray:
    """Rasterise a high-contrast chessboard with a white quiet zone.

    A light-grey background (e.g. 245) makes OpenCV's CALIB_CB_NORMALIZE_IMAGE
    detector fail; 255/0 with a white border is detected for all poses.
    """
    rvec = np.asarray(pose["rvec"], float)
    tvec = np.asarray(pose["tvec"], float)
    image = np.full((HEIGHT, WIDTH), 255, np.uint8)
    for row in range(-1, ROWS):
        for col in range(-1, COLS):
            square = np.array([[col, row, 0], [col + 1, row, 0],
                               [col + 1, row + 1, 0], [col, row + 1, 0]], float) * SQUARE_M
            polygon = cv2.projectPoints(square, rvec, tvec, K_TRUE, D_TRUE)[0].reshape(-1, 2)
            cv2.fillConvexPoly(image, np.rint(polygon).astype(np.int32),
                               255 if (col + row) % 2 == 0 else 0)
    return cv2.GaussianBlur(image, (0, 0), 0.65)


def png_base64(image: np.ndarray) -> str:
    return base64.b64encode(cv2.imencode(".png", image)[1].tobytes()).decode("ascii")


def spike_stream(image: np.ndarray, ticks: int, rng: np.random.Generator) -> np.ndarray:
    """Independent Bernoulli firing per tick; bright/dark photon flux differs."""
    probability = 0.02 + 0.80 * image.astype(np.float32) / 255.0
    stream = np.empty((ticks, HEIGHT, WIDTH), np.uint8)
    for tick in range(ticks):
        stream[tick] = (rng.random((HEIGHT, WIDTH)) < probability).astype(np.uint8)
    return stream


def true_rays(pixels) -> list[dict]:
    undistorted = cv2.undistortPoints(np.asarray(pixels, float)[:, None, :], K_TRUE, D_TRUE).reshape(-1, 2)
    rays = np.column_stack((undistorted, np.ones(len(undistorted))))
    rays /= np.linalg.norm(rays, axis=1)[:, None]
    return [{"pixel": list(map(float, p)),
             "azimuth_deg": float(np.degrees(np.arctan2(r[0], r[2]))),
             "elevation_deg": float(np.degrees(np.arctan2(-r[1], np.hypot(r[0], r[2]))))}
            for p, r in zip(pixels, rays)]


def ray_to_pixel(angle_deg: float) -> list[float]:
    ray = np.array([[math.tan(math.radians(angle_deg)), 0.0, 1.0]])
    return cv2.projectPoints(ray, np.zeros(3), np.zeros(3), K_TRUE, D_TRUE)[0].reshape(2).tolist()


def turntable_payload(mode: str) -> dict:
    observations = []
    for split, encoders in (("train", TURNTABLE_TRAIN), ("test", TURNTABLE_TEST)):
        for encoder in encoders:
            if mode == "far_field":
                expected = -encoder
            else:
                q = math.radians(-encoder)
                rotation = np.array([[math.cos(q), 0.0, math.sin(q)], [0.0, 1.0, 0.0],
                                     [-math.sin(q), 0.0, math.cos(q)]])
                point = rotation @ FINITE_TARGET - FINITE_OFFSET
                expected = math.degrees(math.atan2(point[0], point[2]))
            # observed = expected - zero  =>  estimated zero = TURNTABLE_ZERO_DEG
            observations.append({"sequence_id": f"mock-turntable-{mode}-{split}",
                                 "split": split, "encoder_deg": float(encoder),
                                 "pixel": ray_to_pixel(expected - TURNTABLE_ZERO_DEG)})
    reference = {"mode": mode, "sign": 1}
    if mode == "far_field":
        reference.update(reference_angle_deg=0.0, far_field_confirmed=True)
    else:
        reference.update(target_position_m=FINITE_TARGET.tolist(),
                         camera_offset_m=FINITE_OFFSET.tolist())
    return {"observations": observations, "reference": reference,
            "zero_offset_deg_truth": TURNTABLE_ZERO_DEG}


def capture(name: str, sequence_id: str, split: str, image_points, preview: str | None) -> dict:
    result = {"id": hashlib.sha256(f"mock-v1:{name}:{sequence_id}".encode()).hexdigest(),
              "name": name, "sequence_id": sequence_id, "split": split,
              "evidence": "synthetic", "enabled": True, "found": True,
              "image_size": [WIDTH, HEIGHT], "object_points": OBJ.tolist(),
              "image_points": np.asarray(image_points, float).tolist(),
              "source": {"kind": "independent_mock_projection", "seed": SEED,
                         "coordinate_domain": "transformed_pixels",
                         "note": "外部生成的已知真值角点；不来自标定软件自身"}}
    if preview:
        result["preview_base64"] = preview
    return result


def scenario_observations(root: Path, name: str, poses: list[dict], transform, preview_cache: dict) -> dict:
    train, test = [], []
    for pose in poses:
        index = pose["index"]
        truth = np.asarray(pose["image_points_truth"], float)
        points = transform(truth, index)
        split = "train" if index < 24 else "test"
        entry = capture(f"{name}_{index:02d}.json", f"mock-{name}-{split}-{index:02d}",
                        split, points, preview_cache.get(index))
        (train if split == "train" else test).append(entry)
    train_file = root / "scenes" / name / "observations_train.json"
    test_file = root / "scenes" / name / "observations_test.json"
    write_json(train_file, {"captures": train})
    write_json(test_file, {"captures": test})
    return {"id": name, "kind": "observations",
            "files": [{"path": str(train_file.relative_to(root)).replace("\\", "/"),
                       "api": "observations", "split": "train"},
                      {"path": str(test_file.relative_to(root)).replace("\\", "/"),
                       "api": "observations", "split": "test"}]}


SCENARIO_NOTES = {
    "clean": {"purpose": "0.04 px 角点噪声、32 个倾斜姿态、视场全覆盖；精度基线",
              "expected": "解出的 K/D 应贴近 ground_truth，训练 RMS 约 0.06 px，独立验证 passed"},
    "noisy": {"purpose": "同一组姿态、0.35 px 角点噪声；检验阈值门禁与参数漂移",
              "expected": "重投影 P95 超过 0.5 px 阈值，独立验证 failed；内参漂移明显但仍可解"},
    "prism": {"purpose": "叠加 5 参数模型无法表达的薄棱镜/像散残差",
              "expected": "重投影误差依旧很小（可通过阈值），但视线角出现系统性偏差——几何自洽不等于绝对精度"},
    "degenerate": {"purpose": "全部正对相机、无畸变、无角点噪声；严格秩亏的退化姿态",
                   "expected": "软件以『姿态约束退化』拒绝求解"},
    "degenerate_noisy": {"purpose": "同样的严格退化姿态，仅叠加 0.04 px 角点噪声",
                         "expected": "退化拦截不再触发；求解器给出 fx≈7万 px 的荒谬焦距，intrinsic_std 偶发 NaN 导致工程无法序列化、HTTP 报错；同时暴露防护盲区与数值健壮性缺陷"},
    "flat_distorted": {"purpose": "全部正对相机但保留真实径向畸变",
                       "expected": "软件不会拦截（畸变让单应矩阵带透视项），但内参与指向角显著偏离真值；说明姿态多样性不可省"},
    "leak": {"purpose": "验证集复用训练角点，仅改文件名与序列名",
             "expected": "软件以『训练与验证包含完全相同的角点观测』拒绝求解"},
    "png": {"purpose": "12 张渲染棋盘图（8 训练 + 4 验证），走完整图像检测链路",
            "expected": "12/12 检出 54 个角点，检测误差约 0.8 px，可直接求解与独立验证"},
    "spike_npy": {"purpose": "3 组二值 [T,H,W] Spike 原始脉冲，计数重建",
                  "expected": "3/3 检出角点；文件较大，必须逐个单独导入"},
    "packed_dat": {"purpose": "位打包 DAT，显式声明 packed_flat / little 布局",
                   "expected": "正常解码并检出角点"},
    "window_limit": {"purpose": "同一 DAT，窗口取 384 tick 使解码元素数超过一亿",
                     "expected": "软件以『重建时间窗超出数据范围或超过一亿个脉冲元素』拒绝导入"},
}


def prism_warp(points: np.ndarray) -> np.ndarray:
    """Thin-prism / astigmatic residual outside the Brown-Conrady 5-parameter model."""
    u, v = points[:, 0] - K_TRUE[0, 2], points[:, 1] - K_TRUE[1, 2]
    return np.column_stack((points[:, 0] + 6.0e-6 * u * u - 3.0e-6 * v * v,
                            points[:, 1] + 4.0e-6 * u * v))


def build(root: Path, *, skip_binary: bool) -> dict:
    rng = np.random.default_rng(SEED)
    poses = [make_pose(rng, index) for index in range(32)]
    preview_cache = {p["index"]: png_base64(render_board(p)) for p in poses}
    scenes: list[dict] = []

    clean_noise = {p["index"]: rng.normal(0.0, 0.04, (len(OBJ), 2)) for p in poses}
    noisy_noise = {p["index"]: rng.normal(0.0, 0.35, (len(OBJ), 2)) for p in poses}

    scenes.append(scenario_observations(root, "clean", poses,
                                        lambda truth, i: truth + clean_noise[i], preview_cache))
    scenes.append(scenario_observations(root, "noisy", poses,
                                        lambda truth, i: truth + noisy_noise[i], preview_cache))
    scenes.append(scenario_observations(root, "prism", poses,
                                        lambda truth, i: prism_warp(truth + clean_noise[i]), preview_cache))

    # Negative case 1: front-facing boards, no distortion, no noise -> exact rank deficit.
    flat = [make_pose(rng, index, front_facing=True, distortion=np.zeros(5)) for index in range(32)]
    scenes.append(scenario_observations(root, "degenerate", flat,
                                        lambda truth, i: truth, {}))
    # Guard blind spot: the same exact rank deficit + 0.04 px corner noise.
    # Numerical noise gives the homography a tiny perspective term, so the guard
    # no longer fires and the solver reports a physically meaningless focal length.
    scenes.append(scenario_observations(root, "degenerate_noisy", flat,
                                        lambda truth, i: truth + clean_noise[i], {}))
    # Observation case: the same front-facing boards WITH radial distortion.
    # The homography rank guard does not fire here, yet the intrinsics are biased.
    flat_distorted = [make_pose(rng, index, front_facing=True) for index in range(32)]
    scenes.append(scenario_observations(root, "flat_distorted", flat_distorted,
                                        lambda truth, i: truth + clean_noise[i], {}))
    # Negative case 2: identical test corners under new names/sequences.
    leak_train = [capture(f"leak_{i:02d}.json", f"mock-leak-train-{i:02d}", "train",
                          np.asarray(poses[i]["image_points_truth"]) + clean_noise[i], preview_cache[i])
                  for i in range(24)]
    leak_test = [capture(f"leak_test_{i:02d}.json", f"mock-leak-test-{i:02d}", "test",
                         np.asarray(poses[i]["image_points_truth"]) + clean_noise[i], preview_cache[i])
                 for i in range(8)]
    leak_dir = root / "scenes" / "leak"
    write_json(leak_dir / "observations_train.json", {"captures": leak_train})
    write_json(leak_dir / "observations_test.json", {"captures": leak_test})
    scenes.append({"id": "leak", "kind": "observations",
                   "files": [{"path": "scenes/leak/observations_train.json", "api": "observations", "split": "train"},
                             {"path": "scenes/leak/observations_test.json", "api": "observations", "split": "test"}]})

    # Image-file pipeline: 8 train + 4 test rendered chessboards.
    png_dir = root / "files" / "png"
    png_files = []
    for pose in poses[:8] + poses[24:28]:
        index = pose["index"]
        split = "train" if index < 24 else "test"
        path = png_dir / f"pose_{index:02d}.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        cv2.imwrite(str(path), render_board(pose))
        png_files.append({"path": str(path.relative_to(root)).replace("\\", "/"), "split": split,
                          "sequence_id": f"mock-png-{split}-{index:02d}",
                          "options": {"black_level": 0, "white_level": 255}})
    scenes.append({"id": "png", "kind": "files", "files": png_files})

    if not skip_binary:
        npy_dir = root / "files" / "spike_npy"
        npy_files = []
        for pose in poses[:3]:
            index = pose["index"]
            image = render_board(pose)
            stream = spike_stream(image, SPIKE_TICKS_NPY, rng)
            path = npy_dir / f"spike_pose_{index:02d}.npy"
            path.parent.mkdir(parents=True, exist_ok=True)
            np.save(path, stream, allow_pickle=False)
            npy_files.append({"path": str(path.relative_to(root)).replace("\\", "/"), "split": "train",
                              "sequence_id": f"mock-npy-train-{index:02d}",
                              "options": {"method": "count", "window_start_tick": 0,
                                          "window_ticks": SPIKE_TICKS_NPY,
                                          "period_s": 1.0 / SAMPLE_RATE_HZ,
                                          "black_level": 0, "white_level": SPIKE_TICKS_NPY}})
        scenes.append({"id": "spike_npy", "kind": "files", "files": npy_files})

        dat_dir = root / "files" / "packed_dat"
        dat_dir.mkdir(parents=True, exist_ok=True)
        stream = spike_stream(render_board(poses[0]), SPIKE_TICKS_DAT, rng)
        packed = np.packbits(stream.reshape(SPIKE_TICKS_DAT, -1), axis=1, bitorder="little")
        path = dat_dir / "spike_pose_00.dat"
        path.write_bytes(packed.tobytes())
        dat_options = {"layout_confirmed": True, "layout": "packed_flat", "bitorder": "little",
                       "width": WIDTH, "height": HEIGHT, "header_bytes": 0,
                       "frame_stride_bytes": FRAME_STRIDE_BYTES, "method": "count",
                       "window_start_tick": 0, "window_ticks": 256,
                       "black_level": 0, "white_level": 256}
        scenes.append({"id": "packed_dat", "kind": "files",
                       "files": [{"path": str(path.relative_to(root)).replace("\\", "/"),
                                 "split": "train", "sequence_id": "mock-dat-train-00",
                                 "options": dat_options}]})
        scenes.append({"id": "window_limit", "kind": "files",
                       "files": [{"path": str(path.relative_to(root)).replace("\\", "/"),
                                 "split": "train", "sequence_id": "mock-dat-overlimit-00",
                                 "options": {**dat_options, "window_ticks": SPIKE_TICKS_DAT}}]})

    truth = {
        "note": "外部独立生成的已知真值。不要导入标定软件；仅供 check_mock_calibration.py 对照。",
        "seed": SEED, "image_size": [WIDTH, HEIGHT], "target": TARGET,
        "K": K_TRUE.tolist(), "distortion": D_TRUE.tolist(),
        "sample_rate_hz": SAMPLE_RATE_HZ, "corner_noise_sigma_px": {"clean": 0.04, "noisy": 0.35},
        "object_points": OBJ.tolist(),
        "poses": [{"index": p["index"], "split": "train" if p["index"] < 24 else "test",
                   "rvec": p["rvec"], "tvec": p["tvec"], "depth_m": p["depth_m"],
                   "board_center_px": p["board_center_px"],
                   "image_points_truth": p["image_points_truth"]} for p in poses],
        "flat_poses": [{"index": p["index"], "image_points_truth": p["image_points_truth"]}
                       for p in flat_distorted],
        "query_rays": true_rays(QUERY_PIXELS),
        "turntable": {"zero_offset_deg": TURNTABLE_ZERO_DEG,
                      "far_field": turntable_payload("far_field"),
                      "finite_distance": turntable_payload("finite_distance")},
    }
    write_json(root / "ground_truth.json", truth)

    for mode in ("far_field", "finite_distance"):
        write_json(root / "turntable" / f"{mode}.json", truth["turntable"][mode])

    manifest = {
        "schema": "spike-mock-calibration-data/1",
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "truth_file": "ground_truth.json",
        "software_config": {
            "target": TARGET,
            "orientation": {"rotation_deg": 0, "flip_x": False, "flip_y": False},
            "acceptance": {"intrinsics_p95_px": 0.5, "turntable_p95_deg": 0.05},
            "operator": "mock-test",
        },
        "conventions": {
            "evidence": "所有模拟数据必须标为 synthetic，软件会给出『不能作为实机精度验收』的警告",
            "sequence_id": "train 与 test 必须使用不同采集序列；同一序列不能跨两种用途",
            "files": "大文件请逐个导入（单个 HTTP 请求上限 64 MiB，Base64 约有 1/3 膨胀）",
            "npy": f"二值 [T,{HEIGHT},{WIDTH}] uint8，T={SPIKE_TICKS_NPY}；计数重建窗 {SPIKE_TICKS_NPY / SAMPLE_RATE_HZ * 1000:.1f} ms @ {SAMPLE_RATE_HZ / 1000:.0f} kHz",
            "dat": f"packed_flat / little / {WIDTH}x{HEIGHT} / 帧步长 {FRAME_STRIDE_BYTES} 字节 / 无文件头 / {SPIKE_TICKS_DAT} 帧",
            "window_limit": f"解码后元素数 ticks*W*H 不得超过 1e8；{WIDTH}x{HEIGHT} 最多 {int(1e8 // (WIDTH * HEIGHT))} tick（{1e8 / (WIDTH * HEIGHT) / SAMPLE_RATE_HZ * 1000:.1f} ms @ 20 kHz）",
        },
        "scenarios": scenes,
        "scenario_notes": SCENARIO_NOTES,
        "ui_hints": {
            "config": "工程配置：靶标 chessboard / 9 列 / 6 行 / 格长 0.030 m；方向 0°、不翻转；验收阈值内参 0.5 px、转台 0.05°",
            "observations": "角点 JSON 单独一批导入，不要和图像/Spike 混批；JSON 内的 split/sequence_id/evidence 会覆盖界面填写值",
            "png": "PNG 导入无需改重建选项；黑白电平界面未提供，后端自动拉伸即可",
            "spike_npy": "每个 NPY 单独一批（约 38 MiB，接近前端每批 40 MiB 上限）；『时间窗长度』必须填 128，前端默认 64 会截断一半脉冲；起始 0、方法 count、采样周期 5e-5 秒",
            "packed_dat": "DAT：勾选『确认真实位布局』，layout=packed_flat、bitorder=little、宽 640、高 480、文件头 0、帧步长 38400（可留空由后端取最小值）、时间窗长度 256",
            "window_limit": "同 packed_dat，仅把时间窗长度改为 384；应被后端以『超过一亿个脉冲元素』拒绝",
            "turntable": "转台关系：远场模式勾选确认、参考角 0、sign=1；有限距离模式填目标 [0.30, 0.04, 1.75] m、入口瞳偏置 [0.12, 0.01, 0.06] m，观测取自 turntable/*.json",
        },
        "usage": {
            "automatic": "python scripts/check_mock_calibration.py --data output/mock_calibration_data",
            "manual": [
                "启动：python -m spike_calibration_tool serve --port 8776 --open",
                "新建工程，在『工程配置』把标定板设为 9×6 内角点、格长 0.030 m，方向保持不旋转/不镜像",
                "按 manifest.json 的 scenarios 逐个导入：observations 类走『观测 JSON 导入』，files 类走『文件导入』并照抄对应 options",
                "train/test 分别指定独立的采集序列，来源选『合成示例』",
                "依次点击『求解内参』『独立验证』『转台关系』『视线查询』，并导出 JSON/HTML 报告",
                "用 ground_truth.json 对照内参、畸变、转台零位和视线角；各场景预期见 scenario_notes",
                "degenerate / leak / window_limit 应被软件拒绝，degenerate_noisy 也可能因诊断 NaN 报错；看到报错属于测试通过",
            ],
        },
    }
    write_json(root / "manifest.json", manifest)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="生成带已知真值的 Spike 标定模拟数据")
    parser.add_argument("--output", type=Path, default=Path("output/mock_calibration_data"),
                        help="输出目录（默认 output/mock_calibration_data）")
    parser.add_argument("--skip-binary", action="store_true",
                        help="跳过体量较大的 Spike NPY / DAT，仅生成 JSON 与 PNG")
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = build(root, skip_binary=args.skip_binary)
    sizes = sorted((p.stat().st_size, p.relative_to(root).as_posix())
                   for p in root.rglob("*") if p.is_file())
    total = sum(size for size, _ in sizes)
    print(json.dumps({"output": str(root), "scenarios": [s["id"] for s in manifest["scenarios"]],
                      "files": len(sizes), "total_MiB": round(total / 1024 ** 2, 2),
                      "largest_MiB": round(sizes[-1][0] / 1024 ** 2, 2),
                      "note": "ground_truth.json 不导入软件，仅供检查脚本对照"}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
