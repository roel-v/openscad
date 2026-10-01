import base64
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
import uuid
import zlib

spec = importlib.util.spec_from_file_location("client", Path(__file__).parents[1] / "scripts/client.py")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class ClientTests(unittest.TestCase):
    def test_connection_guards(self):
        info = {"product": "OpenSCAD", "protocol": 1, "server_id": str(uuid.uuid4()), "url": "http://127.0.0.1:1234"}
        self.assertEqual(client.connection(info), info)
        for url in ("http://localhost:1234", "https://127.0.0.1:1234", "http://127.0.0.1:1234/remote", "http://x@127.0.0.1:1234", "http://127.0.0.1:1234#foo"):
            with self.assertRaises(ValueError):
                client.connection({**info, "url": url})
        with self.assertRaises(ValueError):
            client.identifier("00000000-0000-0000-0000-000000000000")
        for route in ("https://example.com/v2/info", "/v2/../v1/info", "/v2/info\r\nHost:x", "//example.com/v2/info"):
            with self.assertRaises(ValueError):
                client.request(info, "GET", route)

    def test_receipt_identity_and_uncertain_send(self):
        info = {"server_id": str(uuid.uuid4())}
        request_id = str(uuid.uuid4())
        body = {"request_id": request_id}
        queued = {**body, "server_id": info["server_id"], "state": "running", "execution_started": True}
        success = {**queued, "state": "succeeded", "result": {"ok": True}}
        with patch.object(client, "request", side_effect=[queued, success]) as send, patch.object(client.time, "sleep"), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(client.operation(info, "/v2/capture", body), success)
            self.assertEqual(send.call_args_list[0].args[1], "POST")
            self.assertEqual(send.call_args_list[1].args[2], "/v2/requests/" + request_id)
        for reply in ({**success, "server_id": str(uuid.uuid4())}, {**success, "request_id": str(uuid.uuid4())}, {**success, "state": "expired"}):
            with patch.object(client, "request", return_value=reply), self.assertRaises(client.AgentError), contextlib.redirect_stderr(io.StringIO()):
                client.operation(info, "/v2/capture", body)
        diagnostic = io.StringIO()
        with patch.object(client, "request", side_effect=OSError("lost connection")) as send, self.assertRaises(OSError), contextlib.redirect_stderr(diagnostic):
            client.operation(info, "/v2/capture", body)
        self.assertEqual(send.call_count, 1)
        self.assertIn(request_id, diagnostic.getvalue())

    def test_deadline_never_resubmits(self):
        info = {"server_id": str(uuid.uuid4())}
        body = {"request_id": str(uuid.uuid4())}
        with patch.object(client, "request", return_value={**body, "server_id": info["server_id"], "state": "queued"}) as send, patch.object(client.time, "monotonic", side_effect=[0, 2]), self.assertRaises(client.AgentError), contextlib.redirect_stderr(io.StringIO()):
            client.operation(info, "/v2/capture", body, timeout=1)
        self.assertEqual(send.call_count, 1)

    def test_png_validation_and_explicit_output(self):
        def chunk(kind, payload):
            return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00")) + chunk(b"IEND", b"")
        value = {"png_base64": base64.b64encode(png).decode(), "png_sha256": hashlib.sha256(png).hexdigest(), "width": 1, "height": 1}
        self.assertEqual(client.png_bytes(value), png)
        for change in ({"width": 2}, {"png_sha256": "0" * 64}, {"png_base64": "bad"}):
            with self.assertRaises(ValueError):
                client.png_bytes({**value, **change})
        broken = png[:-1] + bytes([png[-1] ^ 1])
        with self.assertRaises(ValueError):
            client.png_bytes({**value, "png_base64": base64.b64encode(broken).decode(), "png_sha256": hashlib.sha256(broken).hexdigest()})
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "evidence.png"
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                client.output({"result": value}, str(path))
            self.assertEqual(path.read_bytes(), png)
            self.assertNotIn("png_base64", stdout.getvalue())
            self.assertEqual(json.loads(stdout.getvalue())["result"]["image_file"], str(path.resolve()))
            with self.assertRaises(FileExistsError):
                client.output(value, str(path))

    def test_live_discovery_and_redirect_rejection(self):
        connections = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                connections.append(self.path)
                if self.path != "/v2/info":
                    self.send_response(302)
                    self.send_header("Location", "http://example.invalid/")
                    self.end_headers()
                    return
                data = json.dumps(self.server.info).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        servers = []
        try:
            with tempfile.TemporaryDirectory() as folder:
                directory = Path(folder) / "AgentAnnotations/OpenSCAD"
                directory.mkdir(parents=True)
                for _ in range(2):
                    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                    server.info = {"product": "OpenSCAD", "protocol": 1, "api_version": 2, "access_mode": "agent", "server_id": str(uuid.uuid4()), "url": f"http://127.0.0.1:{server.server_port}"}
                    servers.append(server)
                    threading.Thread(target=server.serve_forever, daemon=True).start()
                    (directory / (server.info["server_id"] + ".json")).write_text(json.dumps(server.info))
                stale = {**servers[0].info, "server_id": str(uuid.uuid4())}
                (directory / "stale.json").write_text(json.dumps(stale))
                with patch.object(client.sys, "platform", "win32"), patch.dict(os.environ, {"LOCALAPPDATA": folder}):
                    self.assertEqual(len(client.discover()["instances"]), 2)
                with self.assertRaises(client.AgentError):
                    client.verify(stale)
                with self.assertRaises(client.AgentError):
                    client.request(servers[0].info, "GET", "/v2/redirect")
                self.assertEqual(connections[-1], "/v2/redirect")
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    unittest.main()
