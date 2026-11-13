"""Decode explicitly supplied capture bytes, retaining raw data semantics."""
from __future__ import annotations

import base64
import hashlib
import io
import math
import uuid
from pathlib import PureWindowsPath

import cv2
import numpy as np

from calibration_system.targets import detect_target, image_quality, spike_representation
from .engine import validate_config, validate_capture


def apply_orientation(image, orientation):
    value = np.rot90(image, -(orientation["rotation_deg"]//90))
    if orientation["flip_x"]:
        value = np.flip(value, axis=1)
    if orientation["flip_y"]:
        value = np.flip(value, axis=0)
    return np.ascontiguousarray(value)


def orient_points(points, image_size, orientation):
    xy = np.asarray(points, dtype=float).copy()
    width, height = image_size
    for _ in range(orientation["rotation_deg"]//90):
        xy = np.column_stack((height-1-xy[:, 1], xy[:, 0]))
        width, height = height, width
    if orientation["flip_x"]:
        xy[:, 0] = width-1-xy[:, 0]
    if orientation["flip_y"]:
        xy[:, 1] = height-1-xy[:, 1]
    return xy, [int(width), int(height)]


def _integer(options, key, default=None, minimum=0):
    value = options.get(key, default)
    if type(value) is not int or value < minimum:
        raise ValueError(f"{key} 必须为不小于 {minimum} 的整数")
    return value


def _window_bounds(options, total_ticks, spatial_pixels):
    start = _integer(options, "window_start_tick", 0)
    default_ticks = min(256, total_ticks-start, 100_000_000//spatial_pixels)
    ticks = _integer(options, "window_ticks", default_ticks, 1)
    if start+ticks > total_ticks or ticks*spatial_pixels > 100_000_000:
        raise ValueError("重建时间窗超出数据范围或超过一亿个脉冲元素")
    return start, ticks


def _raw_window(data, options):
    if data.ndim != 3 or not all(data.shape) or data.size > 100_000_000 or data.dtype.kind not in "buif":
        raise ValueError("Spike NPY 必须是二值 [T,H,W] 数组，最多一亿个元素")
    if data.shape[1]*data.shape[2] > 25_000_000:
        raise ValueError("Spike 图像超过2500万像素")
    start, ticks = _window_bounds(options, len(data), data.shape[1]*data.shape[2])
    window = data[start:start+ticks]
    if not np.all((window == 0) | (window == 1)):
        raise ValueError("Spike NPY 所选时间窗必须为0/1二值，不能把灰度视频当原始脉冲")
    representation = spike_representation(window, options.get("method", "count"), options.get("period_s"))
    return representation["image"], {"kind": "raw_spike_window", "method": representation["method"], "unit": representation["unit"],
        "window_start_tick": start, "window_ticks": ticks, "period_s": representation["period_s"],
        "observation_semantics": representation["observation_semantics"], "raw_data_modified": False,
        "spike_quality": representation["quality"]}


def _check_npy_header(binary):
    """Check declared allocation against actual bytes before numpy allocates."""
    if binary.startswith(b"PK"):
        raise ValueError("NPY 必须为单个数组；不能将 NPZ 归档改名为 NPY")
    stream = io.BytesIO(binary)
    try:
        version = np.lib.format.read_magic(stream)
        if version == (1, 0):
            shape, _, dtype = np.lib.format.read_array_header_1_0(stream)
        elif version in ((2, 0), (3, 0)):
            # Allowed scalar numeric dtype/shape fields are ASCII also in v3.
            shape, _, dtype = np.lib.format.read_array_header_2_0(stream)
        else:
            raise ValueError("不支持的 NPY 版本")
    except (ValueError, EOFError, OSError) as exc:
        raise ValueError("无法读取二值 Spike NPY 头") from exc
    if len(shape) != 3 or not all(n > 0 for n in shape) or dtype.kind not in "buif":
        raise ValueError("Spike NPY 必须是数值二值 [T,H,W] 数组")
    count = math.prod(shape)
    if count > 100_000_000 or shape[1]*shape[2] > 25_000_000:
        raise ValueError("Spike NPY 声明尺寸超过元素/图像上限")
    if count*dtype.itemsize != len(binary)-stream.tell():
        raise ValueError("Spike NPY 头声明的数据长度与文件不一致")


def _packed(data, options):
    if options.get("layout_confirmed") is not True or options.get("layout") not in ("packed_flat", "row_packed") or options.get("bitorder") not in ("little", "big"):
        raise ValueError("DAT/BIN 不猜测布局：请明确 layout_confirmed=true、layout=packed_flat/row_packed 和 bitorder=little/big")
    width, height = _integer(options, "width", minimum=1), _integer(options, "height", minimum=1)
    if width*height > 25_000_000:
        raise ValueError("DAT/BIN图像尺寸超过2500万像素")
    row_bytes = (width+7)//8
    minimum = (width*height+7)//8 if options["layout"] == "packed_flat" else row_bytes*height
    stride = _integer(options, "frame_stride_bytes", minimum, minimum)
    header = _integer(options, "header_bytes", 0)
    if header >= len(data) or (len(data)-header) % stride:
        raise ValueError("文件长度与所填头长度/帧步长不匹配；请核实厂商布局")
    total = (len(data)-header)//stride
    start, ticks = _window_bounds(options, total, width*height)
    raw = np.frombuffer(data, dtype=np.uint8, offset=header+start*stride, count=ticks*stride).reshape(ticks, stride)[:, :minimum]
    if options["layout"] == "packed_flat":
        decoded = np.unpackbits(raw, axis=1, bitorder=options["bitorder"])[:, :width*height].reshape(ticks, height, width)
    else:
        decoded = np.unpackbits(raw.reshape(ticks, height, row_bytes), axis=2, bitorder=options["bitorder"])[:, :, :width]
    local = {**options, "window_start_tick": 0, "window_ticks": ticks}
    image, source = _raw_window(decoded, local)
    source.update({"window_start_tick": start, "layout": options["layout"], "bitorder": options["bitorder"],
                   "header_bytes": header, "frame_stride_bytes": stride, "total_ticks": total, "source_image_size": [width, height]})
    return image, source


def _preview_gray(image, options):
    if image.ndim == 3:
        if image.shape[2] not in (3, 4):
            raise ValueError("图像仅支持灰度、BGR或BGRA通道")
        image = cv2.cvtColor(image, cv2.COLOR_BGRA2GRAY if image.shape[2] == 4 else cv2.COLOR_BGR2GRAY)
    if image.ndim != 2 or not image.size or image.size > 25_000_000 or not np.isfinite(image).all():
        raise ValueError("需要有效灰度图像，且不超过2500万像素")
    low = options.get("black_level", float(np.min(image)))
    high = options.get("white_level", float(np.max(image)))
    if isinstance(low, bool) or isinstance(high, bool) or not math.isfinite(float(low)) or not math.isfinite(float(high)):
        raise ValueError("预览黑白电平必须为有限数值")
    if float(high) <= float(low):
        if "black_level" in options or "white_level" in options:
            raise ValueError("预览白电平必须大于黑电平")
        return np.zeros(image.shape, np.uint8)
    return np.clip((image.astype(float)-float(low))/(float(high)-float(low))*255, 0, 255).astype(np.uint8)


def encode_preview(image):
    ok, binary = cv2.imencode(".png", image)
    if not ok:
        raise ValueError("无法编码预览图")
    return base64.b64encode(binary).decode("ascii")


def import_capture(file_record, config, *, split, sequence_id, evidence, options=None):
    config = validate_config(config)
    if not isinstance(file_record, dict) or not isinstance(file_record.get("name"), str):
        raise ValueError("导入文件缺少名称")
    options = options or {}
    if not isinstance(options, dict):
        raise ValueError("导入选项必须为对象")
    name = PureWindowsPath(file_record["name"]).name
    if not name or len(name) > 500:
        raise ValueError("文件名无效")
    content = file_record.get("content_base64")
    if not isinstance(content, str) or len(content) > 90_000_000:
        raise ValueError("文件内容缺失或超过64MiB导入限制")
    try:
        binary = base64.b64decode(content.split(",", 1)[-1], validate=True)
    except (ValueError, TypeError) as exc:
        raise ValueError("文件内容不是有效Base64") from exc
    if not binary or len(binary) > 64*1024**2:
        raise ValueError("文件为空或超过64MiB导入限制")
    suffix = PureWindowsPath(name).suffix.lower()
    if suffix == ".npy":
        _check_npy_header(binary)
        try:
            raw = np.load(io.BytesIO(binary), allow_pickle=False)
        except (ValueError, EOFError, OSError) as exc:
            raise ValueError("无法读取不含对象的二值Spike NPY") from exc
        if not isinstance(raw, np.ndarray):
            if hasattr(raw, "close"):
                raw.close()
            raise ValueError("NPY 必须为单个数组；不能将NPZ归档改名为NPY")
        image, source = _raw_window(raw, options)
    elif suffix in (".dat", ".bin"):
        image, source = _packed(binary, options)
    elif suffix in (".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"):
        image = cv2.imdecode(np.frombuffer(binary, np.uint8), cv2.IMREAD_UNCHANGED)
        if image is None:
            raise ValueError("无法解码图像文件")
        source = {"kind": "image", "note": "导入图像仅用于几何标定，不把8位预览当辐射量"}
    else:
        raise ValueError("支持PNG/JPEG/BMP/TIFF、二值Spike NPY及已知布局DAT/BIN；角点JSON请使用观测导入")
    gray = apply_orientation(_preview_gray(image, options), config["orientation"])
    target = detect_target(gray, config["target"])
    quality = target.get("quality", image_quality(gray))
    metrics = quality.get("metrics", {})
    if "saturated_fraction" in metrics:
        metrics["preview_white_endpoint_fraction"] = metrics.pop("saturated_fraction")
    if "black_fraction" in metrics:
        metrics["preview_black_endpoint_fraction"] = metrics.pop("black_fraction")
    quality["suggestions"] = [s for s in quality.get("suggestions", []) if "剪切" not in s]
    quality["scope"] = "预览灰度已按显示电平拉伸，端点占比不代表传感器饱和；仅作几何角点质量诊断"
    source.update({"sha256": hashlib.sha256(binary).hexdigest(), "bytes": len(binary), "orientation": config["orientation"]})
    capture = {"id": uuid.uuid4().hex, "name": name, "sequence_id": sequence_id, "split": split, "evidence": evidence,
        "enabled": True, "found": target["found"], "image_size": [int(gray.shape[1]), int(gray.shape[0])],
        "object_points": target["object_points"], "image_points": target["points"], "coordinate_domain": "transformed_pixels",
        "quality": quality, "preview_base64": encode_preview(gray), "source": source,
        "reasons": target.get("reasons", [])}
    return validate_capture(capture)
