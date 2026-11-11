"""Command-line and native-window entry points."""
from __future__ import annotations

import argparse
import logging
import os
from pathlib import Path
import sys
import threading
import webbrowser

from . import __version__


def default_data_root():
    if getattr(sys, "frozen", False):
        return Path(os.environ.get("LOCALAPPDATA", Path.home())) / "SpikeCalibrationTool" / "workspace"
    return Path(__file__).resolve().parents[1] / "spike_calibration_workspace"


def configure_logging(root):
    logdir = Path(root) / "logs"
    logdir.mkdir(parents=True, exist_ok=True)
    path = logdir / "tool.log"
    logging.basicConfig(filename=path, level=logging.INFO, encoding="utf-8",
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if sys.stdout is None:
        sys.stdout = path.open("a", encoding="utf-8", buffering=1)
    if sys.stderr is None:
        sys.stderr = sys.stdout


def demo(root, output):
    from .service import Workbench, dumps
    workbench = Workbench(root)
    project = workbench.create_project({"name": "合成几何自检"})
    project = workbench.load_demo(project["id"])
    project = workbench.solve(project["id"])
    project = workbench.validate(project["id"])
    K = project["model"]["K"]
    ray = workbench.query(project["id"], {"points": [[K[0][2], K[1][2]]]})
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "calibration.json").write_text(dumps(workbench.export_project(project["id"])), encoding="utf-8")
    (output / "report.html").write_text(workbench.report_html(project["id"]), encoding="utf-8")
    result = {"app": "spike-calibration-tool", "version": __version__, "project_id": project["id"],
              "evidence": "synthetic", "validation": project["validation"], "query": ray}
    (output / "self_test.json").write_text(dumps(result), encoding="utf-8")
    print(dumps(result))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Spike 单支路标定工具")
    parser.add_argument("--data", type=Path, default=None, help="项目存储目录")
    parser.add_argument("--version", action="version", version=__version__)
    subs = parser.add_subparsers(dest="command")
    serve = subs.add_parser("serve", help="启动本机浏览器工作台")
    serve.add_argument("--port", type=int, default=8776)
    serve.add_argument("--open", action="store_true")
    desktop = subs.add_parser("desktop", help="启动 Windows 原生窗口")
    desktop.add_argument("--port", type=int, default=0)
    for cmd in ("demo", "self-test"):
        item = subs.add_parser(cmd, help="运行明确标注为合成的完整几何自检")
        item.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    root = args.data or default_data_root()
    configure_logging(root)
    if args.command in ("demo", "self-test"):
        return demo(root, args.output)
    if args.command in (None, "desktop"):
        from .desktop import run_desktop
        return run_desktop(root, getattr(args, "port", 0))
    from .server import CalibrationHTTPServer
    from .service import Workbench
    with CalibrationHTTPServer(("127.0.0.1", args.port), Workbench(root)) as server:
        url = f"http://127.0.0.1:{server.server_address[1]}"
        if args.open:
            threading.Timer(0.3, lambda: webbrowser.open(url)).start()
        print(f"Spike 标定工具 {__version__} · {url}", flush=True)
        try:
            server.serve_forever(poll_interval=0.2)
        except KeyboardInterrupt:
            pass
    return 0
