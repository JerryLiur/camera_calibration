"""Same-origin loopback HTTP application, without a web framework dependency."""
from __future__ import annotations

import json
import logging
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from .service import dumps, strict_loads

WEB_ROOT = Path(__file__).parent / "web"
MAX_REQUEST_BYTES = 64 * 1024**2
LOG = logging.getLogger(__name__)


class CalibrationHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, workbench):
        if address[0] != "127.0.0.1":
            raise ValueError("标定工具仅监听 127.0.0.1")
        super().__init__(address, Handler)
        self.workbench = workbench


class Handler(BaseHTTPRequestHandler):
    server_version = "SpikeCalibration/1.0"

    def log_message(self, fmt, *args):
        LOG.info(fmt, *args)

    def _reply(self, value, status=200, content_type="application/json; charset=utf-8", filename=None):
        if isinstance(value, bytes):
            binary = value
        else:
            binary = (value if isinstance(value, str) else dumps(value)).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(binary)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "same-origin")
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; object-src 'none'; frame-ancestors 'none'")
        if filename:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        self.wfile.write(binary)

    def _guard(self):
        port = self.server.server_address[1]
        expected = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if self.headers.get("Host") not in expected:
            raise PermissionError("只允许本机访问")
        origin = self.headers.get("Origin")
        if origin and origin not in {f"http://{host}" for host in expected}:
            raise PermissionError("不允许跨来源访问")
        if self.headers.get("Sec-Fetch-Site") == "cross-site":
            raise PermissionError("不允许跨站访问")

    def _body(self):
        if self.headers.get("Content-Type", "").split(";", 1)[0] != "application/json":
            raise ValueError("请求需要 application/json")
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 < length <= MAX_REQUEST_BYTES:
            raise ValueError("请求大小必须为 1 字节至 64 MiB")
        payload = self.rfile.read(length)
        if len(payload) != length:
            raise ValueError("请求体不完整")
        result = strict_loads(payload.decode("utf-8"))
        if not isinstance(result, dict):
            raise ValueError("请求内容必须为 JSON 对象")
        return result

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_PATCH(self):
        self._dispatch("PATCH")

    def _dispatch(self, method):
        try:
            self._guard()
            split = urlsplit(self.path)
            path = split.path.rstrip("/") or "/"
            query = parse_qs(split.query)
            body = self._body() if method != "GET" else {}
            response = self._route(method, path, query, body)
            if response is not None:
                self._reply(response)
        except PermissionError as exc:
            self._reply({"error": str(exc), "code": "forbidden"}, 403)
        except KeyError as exc:
            self._reply({"error": str(exc), "code": "not_found"}, 404)
        except (ValueError, TypeError, IndexError, UnicodeError, OverflowError) as exc:
            self._reply({"error": str(exc), "code": "invalid_request"}, 400)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:
            LOG.exception("Calibration request failed")
            self._reply({"error": "处理失败，请检查本机日志和输入数据", "code": "operation_failed"}, 500)

    def _route(self, method, path, query, body):
        app = self.server.workbench
        if method == "GET" and path in ("/", "/index.html", "/app.js", "/styles.css"):
            file = WEB_ROOT / ("index.html" if path in ("/", "/index.html") else path[1:])
            content_type = {".html": "text/html", ".js": "text/javascript", ".css": "text/css"}[file.suffix]
            self._reply(file.read_bytes(), content_type=content_type+"; charset=utf-8")
            return None
        if method == "GET" and path == "/favicon.ico":
            self._reply(b"", 204, "image/x-icon")
            return None
        if method == "GET" and path == "/api/state":
            return app.state()
        if method == "POST" and path == "/api/projects":
            return app.create_project(body)
        parts = path.strip("/").split("/")
        if len(parts) < 3 or parts[:2] != ["api", "projects"]:
            raise KeyError("接口不存在")
        project_id = parts[2]
        if method == "GET" and len(parts) == 3:
            return app.get_project(project_id)
        action = parts[3] if len(parts) > 3 else ""
        if method == "PATCH" and action == "config" and len(parts) == 4:
            return app.update_config(project_id, body)
        if method == "PATCH" and action == "captures" and len(parts) == 5:
            return app.update_capture(project_id, parts[4], body)
        if method == "POST" and len(parts) == 4:
            if action == "demo":
                return app.load_demo(project_id)
            if action == "solve":
                return app.solve(project_id)
            if action == "validate":
                return app.validate(project_id)
            functions = {"import": app.import_files, "observations": app.import_observations,
                         "turntable": app.calibrate_turntable, "query": app.query}
            if action in functions:
                return functions[action](project_id, body)
        if method == "GET":
            if action == "preview" and len(parts) == 5:
                self._reply(app.preview(project_id, parts[4], query.get("undistort") == ["1"]), content_type="image/png")
                return None
            if action == "export" and len(parts) == 4:
                self._reply(app.export_project(project_id), filename=f"spike-calibration-{project_id[:8]}.json")
                return None
            if action == "report" and len(parts) == 4:
                self._reply(app.report_html(project_id), content_type="text/html; charset=utf-8", filename=f"spike-report-{project_id[:8]}.html")
                return None
        raise KeyError("接口不存在")
