"""Camera and common-plane registration calibration.

Homographies here map *undistorted Spike pixels* to *undistorted RGB
pixels*. They do not model stereo parallax, rolling shutter or temporal
misalignment. A successful fit is not an independent accuracy validation.
"""

from __future__ import annotations

from dataclasses import dataclass
from math import ceil
from typing import Sequence

import cv2
import numpy as np


def _points(
    values: np.ndarray, name: str, minimum: int = 1
) -> np.ndarray:
    values = np.asarray(values, dtype=np.float64)
    if (
        values.ndim != 2
        or values.shape[1] != 2
        or len(values) < minimum
    ):
        raise ValueError(
            f"{name} must have finite shape [N,2], N >= {minimum}"
        )
    if not np.isfinite(values).all():
        raise ValueError(f"{name} must be finite")
    return values


def _size(size: tuple[int, int]) -> tuple[int, int]:
    if len(size) != 2 or any(
        isinstance(x, (bool, np.bool_))
        or not isinstance(x, (int, np.integer))
        or x <= 0
        for x in size
    ):
        raise ValueError(
            "image_size must be positive integer (width,height)"
        )
    return int(size[0]), int(size[1])


@dataclass(frozen=True)
class CameraModel:
    """OpenCV pinhole model at one sensor resolution, with explicit provenance."""

    K: np.ndarray
    distortion: np.ndarray
    image_size: tuple[int, int]
    provenance: str

    def __post_init__(self) -> None:
        K = np.asarray(self.K, np.float64).copy()
        distortion = (
            np.asarray(self.distortion, np.float64).reshape(-1).copy()
        )
        if (
            K.shape != (3, 3)
            or not np.isfinite(K).all()
            or K[0, 0] <= 0
            or K[1, 1] <= 0
            or not np.allclose(K[2], [0, 0, 1])
            or K[0, 1] != 0
            or K[1, 0] != 0
        ):
            raise ValueError(
                "K must be a finite standard pinhole intrinsic matrix"
            )
        if (
            len(distortion) not in (4, 5, 8, 12, 14)
            or not np.isfinite(distortion).all()
        ):
            raise ValueError(
                "distortion must contain 4,5,8,12 or 14 finite"
                " coefficients"
            )
        if (
            not isinstance(self.provenance, str)
            or not self.provenance.strip()
        ):
            raise ValueError("camera provenance is required")
        K.setflags(write=False)
        distortion.setflags(write=False)
        object.__setattr__(self, "K", K)
        object.__setattr__(self, "distortion", distortion)
        object.__setattr__(self, "image_size", _size(self.image_size))

    def undistort_points(self, xy: np.ndarray) -> np.ndarray:
        points = _points(xy, "xy")
        criteria = (
            cv2.TERM_CRITERIA_COUNT + cv2.TERM_CRITERIA_EPS,
            50,
            1e-12,
        )
        # OpenCV 5 folded the Iter overload into undistortPoints; 4.x uses Iter.
        if hasattr(cv2, "undistortPointsIter"):
            result = cv2.undistortPointsIter(
                points.reshape(-1, 1, 2),
                self.K,
                self.distortion,
                None,
                self.K,
                criteria,
            )
        else:
            result = cv2.undistortPoints(
                points.reshape(-1, 1, 2),
                self.K,
                self.distortion,
                P=self.K,
                criteria=criteria,
            )
        return result.reshape(-1, 2)

    def distort_points(self, undistorted_xy: np.ndarray) -> np.ndarray:
        """Forward distortion used to build an inverse image resampling map."""
        xy = _points(undistorted_xy, "undistorted_xy")
        normalized = np.column_stack(
            (
                (xy[:, 0] - self.K[0, 2]) / self.K[0, 0],
                (xy[:, 1] - self.K[1, 2]) / self.K[1, 1],
                np.ones(len(xy)),
            )
        )
        result, _ = cv2.projectPoints(
            normalized,
            np.zeros(3),
            np.zeros(3),
            self.K,
            self.distortion,
        )
        return result.reshape(-1, 2)

    def undistort_image(self, image: np.ndarray) -> np.ndarray:
        image = np.asarray(image)
        if image.shape[:2] != self.image_size[::-1]:
            raise ValueError(
                "image resolution differs from the camera calibration"
            )
        return cv2.undistort(
            image, self.K, self.distortion, None, self.K
        )


def _homography(matrix: np.ndarray) -> np.ndarray:
    H = np.asarray(matrix, np.float64)
    if (
        H.shape != (3, 3)
        or not np.isfinite(H).all()
        or np.linalg.matrix_rank(H) < 3
    ):
        raise ValueError(
            "homography must be a finite invertible 3x3 matrix"
        )
    return H / (H[2, 2] if abs(H[2, 2]) > 1e-12 else np.linalg.norm(H))


def project_points(
    xy: np.ndarray, homography: np.ndarray
) -> np.ndarray:
    """Apply an explicit pixel-domain homography; reject a projective pole."""
    xy = _points(xy, "xy")
    H = _homography(homography)
    result = np.column_stack((xy, np.ones(len(xy)))) @ H.T
    if np.any(np.abs(result[:, 2]) < 1e-12):
        raise ValueError("points map to a projective pole")
    return result[:, :2] / result[:, 2, None]


def error_summary(errors: np.ndarray) -> dict:
    errors = np.asarray(errors, np.float64).reshape(-1)
    if (
        not len(errors)
        or not np.isfinite(errors).all()
        or np.any(errors < 0)
    ):
        raise ValueError(
            "errors must contain nonnegative finite values"
        )
    return {
        "count": len(errors),
        "rms_px": float(np.sqrt(np.mean(errors**2))),
        "median_px": float(np.median(errors)),
        "p95_px": float(np.percentile(errors, 95)),
        "p99_px": float(np.percentile(errors, 99)),
        "max_px": float(errors.max()),
    }


@dataclass(frozen=True)
class HomographyFit:
    matrix: np.ndarray
    inlier_mask: np.ndarray
    residuals_px: np.ndarray
    method: str
    ransac_threshold_px: float | None

    def summary(self) -> dict:
        return {
            "H_spike_to_rgb_undistorted": self.matrix.tolist(),
            "method": self.method,
            "coordinate_domain": "undistorted pixels",
            "fit_count": len(self.inlier_mask),
            "inlier_count": int(self.inlier_mask.sum()),
            "inlier_fraction": float(self.inlier_mask.mean()),
            "ransac_threshold_rgb_px": self.ransac_threshold_px,
            "all_fit_residuals": error_summary(self.residuals_px),
            "inlier_fit_residuals": error_summary(
                self.residuals_px[self.inlier_mask]
            ),
        }


def fit_homography(
    spike_xy: np.ndarray,
    rgb_xy: np.ndarray,
    *,
    spike_camera: CameraModel | None = None,
    rgb_camera: CameraModel | None = None,
    robust: bool = True,
    threshold_px: float = 1.0,
    min_inlier_fraction: float = 0.6,
) -> HomographyFit:
    """Fit common-plane registration from matching points, without holdout inputs.

    Supply both camera models for raw/distorted coordinates. Without them,
    inputs must already be in undistorted pixel coordinates. RANSAC's threshold
    is measured in destination RGB pixels; its consensus is refined using OLS.
    """
    source, destination = _points(spike_xy, "spike_xy", 4), _points(
        rgb_xy, "rgb_xy", 4
    )
    if source.shape != destination.shape:
        raise ValueError(
            "corresponding points must have identical shape"
        )
    if (spike_camera is None) != (rgb_camera is None):
        raise ValueError("provide both camera models or neither")
    if spike_camera is not None:
        source = spike_camera.undistort_points(source)
        destination = rgb_camera.undistort_points(destination)
    if not np.isfinite(threshold_px) or threshold_px <= 0:
        raise ValueError("threshold_px must be positive and finite")
    if (
        not np.isfinite(min_inlier_fraction)
        or not 0 < min_inlier_fraction <= 1
    ):
        raise ValueError("min_inlier_fraction must lie in (0,1]")
    for points in (source, destination):
        centered = points - points.mean(axis=0)
        singular = np.linalg.svd(centered, compute_uv=False)
        if singular[0] == 0 or singular[1] / singular[0] < 1e-6:
            raise ValueError(
                "collinear or degenerate calibration points"
            )
    H, mask = cv2.findHomography(
        source,
        destination,
        cv2.RANSAC if robust else 0,
        float(threshold_px),
        maxIters=5000,
        confidence=0.999,
    )
    if H is None or mask is None:
        raise ValueError("homography estimation failed")
    inliers = (
        mask.reshape(-1).astype(bool)
        if robust
        else np.ones(len(source), bool)
    )
    if inliers.sum() < max(4, ceil(len(source) * min_inlier_fraction)):
        raise ValueError("insufficient homography inlier support")
    if robust:
        H, _ = cv2.findHomography(
            source[inliers], destination[inliers], 0
        )
        if H is None:
            raise ValueError("homography consensus refinement failed")
    H = _homography(H)
    residuals = np.linalg.norm(
        project_points(source, H) - destination, axis=1
    )
    return HomographyFit(
        H,
        inliers,
        residuals,
        "RANSAC + consensus OLS" if robust else "OLS",
        float(threshold_px) if robust else None,
    )


def evaluate_homography(
    matrix: np.ndarray,
    spike_xy: np.ndarray,
    rgb_xy: np.ndarray,
    *,
    spike_camera: CameraModel | None = None,
    rgb_camera: CameraModel | None = None,
    max_p95_px: float = 1.0,
) -> dict:
    """Evaluate independent correspondences; the threshold is a research gate."""
    source, destination = _points(spike_xy, "spike_xy"), _points(
        rgb_xy, "rgb_xy"
    )
    if source.shape != destination.shape:
        raise ValueError(
            "corresponding points must have identical shape"
        )
    if (spike_camera is None) != (rgb_camera is None):
        raise ValueError("provide both camera models or neither")
    if spike_camera is not None:
        source, destination = (
            spike_camera.undistort_points(source),
            rgb_camera.undistort_points(destination),
        )
    if not np.isfinite(max_p95_px) or max_p95_px <= 0:
        raise ValueError("max_p95_px must be positive and finite")
    errors = np.linalg.norm(
        project_points(source, matrix) - destination, axis=1
    )
    summary = error_summary(errors)
    summary.update(
        max_p95_px=float(max_p95_px),
        accepted=summary["p95_px"] <= max_p95_px,
        threshold_provenance=(
            "research gate; not client acceptance specification"
        ),
    )
    return summary


@dataclass(frozen=True)
class SpatialCalibration:
    spike_camera: CameraModel
    rgb_camera: CameraModel
    homography: np.ndarray

    def __post_init__(self) -> None:
        H = _homography(self.homography).copy()
        H.setflags(write=False)
        object.__setattr__(self, "homography", H)

    def warp_spike_to_rgb(
        self, image: np.ndarray
    ) -> tuple[np.ndarray, np.ndarray]:
        """Resample raw Spike grid once into undistorted RGB grid plus support mask.

        RGB images must also be undistorted with ``rgb_camera.undistort_image``.
        Invalid support is explicitly masked; zero padding is not a measurement.
        """
        image = np.asarray(image)
        if (
            image.ndim not in (2, 3)
            or image.shape[:2] != self.spike_camera.image_size[::-1]
        ):
            raise ValueError(
                "Spike image resolution differs from calibration"
            )
        width, height = self.rgb_camera.image_size
        yy, xx = np.mgrid[:height, :width]
        destination = np.column_stack((xx.ravel(), yy.ravel()))
        source = project_points(
            destination, np.linalg.inv(self.homography)
        )
        raw_source = self.spike_camera.distort_points(source)
        sw, sh = self.spike_camera.image_size
        support = (
            (raw_source[:, 0] >= 0)
            & (raw_source[:, 0] <= sw - 1)
            & (raw_source[:, 1] >= 0)
            & (raw_source[:, 1] <= sh - 1)
        ).reshape(height, width)
        maps = raw_source.reshape(height, width, 2).astype(np.float32)
        warped = cv2.remap(
            image,
            maps[..., 0],
            maps[..., 1],
            cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=0,
        )
        return warped, support


@dataclass(frozen=True)
class CameraCalibrationFit:
    camera: CameraModel
    rms_px: float
    per_view_rms_px: np.ndarray
    intrinsic_std: np.ndarray | None = None


def fit_camera_intrinsics(
    object_points: Sequence[np.ndarray],
    image_points: Sequence[np.ndarray],
    image_size: tuple[int, int],
    *,
    provenance: str,
) -> CameraCalibrationFit:
    """Estimate five-term pinhole intrinsics from >=6 known planar-board views.

    Reuses the ordinary OpenCV calibration route of the project tutorial.
    Pixel correspondences and board coordinates are observations, not supplied
    K/D. Independent views and pose diversity are still required for validation.
    """
    image_size = _size(image_size)
    if (
        len(object_points) != len(image_points)
        or len(object_points) < 6
    ):
        raise ValueError(
            "at least six matching calibration-board views are required"
        )
    objects, images = [], []
    for obj, xy in zip(object_points, image_points):
        obj, xy = np.asarray(obj, np.float64), _points(
            xy, "image_points", 6
        )
        if (
            obj.shape != (len(xy), 3)
            or not np.isfinite(obj).all()
            or not np.allclose(obj[:, 2], 0)
        ):
            raise ValueError(
                "object_points must be finite planar [N,3] board points"
                " with z=0"
            )
        if (
            np.linalg.matrix_rank(obj[:, :2] - obj[:, :2].mean(axis=0))
            < 2
        ):
            raise ValueError(
                "calibration board must have noncollinear points"
            )
        objects.append(obj.astype(np.float32))
        images.append(xy.astype(np.float32))
    rms, K, d, _, _, intrinsic_std, _, per_view = (
        cv2.calibrateCameraExtended(
            objects,
            images,
            image_size,
            None,
            None,
            flags=0,
            criteria=(
                cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER,
                100,
                1e-9,
            ),
        )
    )
    return CameraCalibrationFit(
        CameraModel(K, d, image_size, provenance),
        float(rms),
        per_view.reshape(-1),
        intrinsic_std.reshape(-1),
    )
