"""Load a frozen exported calibration for a client that already has Spike pixels."""
from pathlib import Path

from .engine import query_rays
from .service import strict_loads, validate_project


class SpikeCalibrationSDK:
    def __init__(self, bundle_path):
        bundle = strict_loads(Path(bundle_path).read_text(encoding="utf-8"))
        if not isinstance(bundle, dict) or bundle.get("format") != "spike-calibration-bundle" or bundle.get("format_version") != 1:
            raise ValueError("不支持的标定包格式")
        self.project = validate_project(bundle.get("project"))
        if not self.project.get("model"):
            raise ValueError("标定包尚无内参模型")
        self.evidence = bundle.get("evidence", [])

    def query(self, points, encoder_deg=None):
        """Pixels must use the same rotation/flip convention as the saved project."""
        return query_rays(self.project["model"], points, self.project.get("turntable"), encoder_deg)

    @property
    def config(self):
        import copy
        return copy.deepcopy(self.project["config"])
