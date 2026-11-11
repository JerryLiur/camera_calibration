"""Windows WebView2 shell. Closing the window closes its own HTTP server."""
from __future__ import annotations

import logging
import os
from pathlib import Path
import re
import tempfile
import threading
from urllib.parse import urlsplit
from urllib.request import urlopen

from .server import CalibrationHTTPServer
from .service import Workbench


class DesktopBridge:
    def __init__(self, origin):
        self._origin = origin
        self._window = None

    def save_export(self, relative_url):
        import webview
        try:
            logging.getLogger(__name__).info("Native export requested: %s", relative_url)
            current = urlsplit(self._window.get_current_url()) if self._window is not None else None
            expected = urlsplit(self._origin)
            if current is None or (current.scheme, current.netloc) != (expected.scheme, expected.netloc) or current.path not in ("", "/", "/index.html"):
                raise ValueError("本机标定页面尚未就绪")
            if not isinstance(relative_url, str) or not re.fullmatch(r"/api/projects/[a-f0-9]{32}/(export|report)", relative_url):
                raise ValueError("仅允许保存本工程标定结果或报告")
            suffix = ".json" if relative_url.endswith("export") else ".html"
            logging.getLogger(__name__).info("Opening native save dialog")
            selection = []

            def choose_destination():
                selection.append(self._window.create_file_dialog(webview.FileDialog.SAVE,
                    directory="E:\\download" if Path("E:/download").is_dir() else "",
                    save_filename="spike-calibration" + suffix,
                    file_types=(f"标定结果 (*{suffix})",)))

            # JS API calls run on a worker thread; WinForms file dialogs need its STA UI thread.
            native = getattr(self._window, "native", None)
            if native is not None and getattr(native, "InvokeRequired", False):
                from System import Action
                native.Invoke(Action(choose_destination))
            else:
                choose_destination()
            selected = selection[0] if selection else None
            if not selected:
                return None
            logging.getLogger(__name__).info("Native save destination selected")
            destination = Path(selected[0]).resolve()
            if not destination.suffix:
                destination = destination.with_suffix(suffix)
            with urlopen(self._origin + relative_url, timeout=120) as response:
                if urlsplit(response.url).netloc != urlsplit(self._origin).netloc:
                    raise ValueError("导出地址不正确")
                binary = response.read()
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=".spike-export-", suffix=".tmp", delete=False) as stream:
                    temporary = Path(stream.name)
                    stream.write(binary)
                os.replace(temporary, destination)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            return {"path": str(destination)}
        except Exception as exc:
            logging.getLogger(__name__).exception("Desktop export failed")
            return {"error": str(exc)}


def run_desktop(root, port=0):
    try:
        import webview
    except ImportError as exc:
        raise RuntimeError("缺少 pywebview，请使用浏览器模式，或在当前环境执行 python -m pip install -r requirements-desktop.txt") from exc
    server = CalibrationHTTPServer(("127.0.0.1", port), Workbench(root))
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True)
    thread.start()
    origin = f"http://127.0.0.1:{server.server_address[1]}"
    bridge = DesktopBridge(origin)
    try:
        bridge._window = webview.create_window("Spike 标定工具 V1.0", origin, js_api=bridge,
            width=1440, height=940, min_size=(1000, 720), background_color="#f4f7fb",
            text_select=True, confirm_close=False)
        webview.start(gui="edgechromium", private_mode=True)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
    return 0
