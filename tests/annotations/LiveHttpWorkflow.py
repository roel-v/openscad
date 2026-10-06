"""Test-only native HTTP acceptance driver; no product client imports or CLI wrapper."""
import hashlib
import http.client
import json
from pathlib import Path
import sys
import time
from urllib.parse import urlsplit
import uuid


def main():
    connection, document, window, viewport, destination = sys.argv[1:]
    info = json.loads(Path(connection).read_text(encoding="utf-8-sig"))
    url = urlsplit(info["url"])
    assert url.scheme == "http" and url.hostname == "127.0.0.1"
    calls = []

    def request(route, body=None, image=False):
        assert route.startswith("/v1/")
        transport = http.client.HTTPConnection("127.0.0.1", url.port, timeout=10)
        headers = {"X-OpenSCAD-Agent": "1", "X-OpenSCAD-Agent-Server": info["server_id"]}
        if body is not None:
            headers["Content-Type"] = "application/json"
        try:
            transport.request("GET" if body is None else "POST", route,
                              None if body is None else json.dumps(body), headers)
            reply = transport.getresponse()
            data = reply.read()
            calls.append({"route": route, "status": reply.status})
            assert reply.status in (200, 202), (route, reply.status, data)
            if image:
                assert reply.getheader("Content-Type") == "image/png"
                assert data.startswith(b"\x89PNG\r\n\x1a\n")
                assert hashlib.sha256(data).hexdigest() == reply.getheader("X-Content-SHA256")
                return data
            return json.loads(data)
        finally:
            transport.close()

    def operation(route, **fields):
        body = {"request_id": str(uuid.uuid4()), "server_id": info["server_id"], **fields}
        value = request(route, body)
        deadline = time.monotonic() + 12
        while value["state"] in ("queued", "running"):
            assert time.monotonic() < deadline, value
            assert value["request_id"] == body["request_id"] and value["server_id"] == info["server_id"]
            time.sleep(value["poll_after_ms"] / 1000)
            value = request(value["status_route"])
        assert value["state"] == "succeeded", value
        return value

    live = request("/v1/info")
    assert live["server_id"] == info["server_id"] and live["api_version"] == 1
    help_doc = request(live["links"]["help"])
    assert any(r["path"] == "/v1/capture" for r in help_doc["routes"])
    assert "$defs" in request(live["links"]["schema"])
    assert any(d["document_id"] == document for d in request("/v1/documents")["documents"])
    target = {"document_id": document, "window_id": window}
    base = "/v1/documents/" + document
    captured = operation(base + "/source-snapshots", **target, origin="disk")["result"]
    snapshot = request("/v1/source-snapshots/" + captured["snapshot_id"])
    assert snapshot["origin"] == "disk" and "cube" in snapshot["text"]
    parsed = operation(base + "/parse", **target, snapshot_id=captured["snapshot_id"])["result"]
    assert parsed["success"]
    tree_route = "/v1/trees/" + parsed["tree_id"]
    tree = request(tree_route)
    root_route = tree_route + "/nodes/" + tree["root_ids"][0]
    request(root_route)
    assert request(root_route + "/children")["children"]
    request(base + "/dependencies")
    request(base + "/diagnostics")
    results = request(base + "/results")["results"]
    assert request("/v1/trees/" + results[-1]["tree_id"])["kind"] == "evaluated"
    view = next(v for v in request("/v1/status")["viewports"] if v["viewport_id"] == viewport)
    guards = {"viewport_id": viewport, "expected_result_id": view["displayed_result_id"],
              "expected_view_revision": view["view_revision"]}
    camera = operation("/v1/viewports/" + viewport + "/view", **target, **guards,
                       operation="standard", view="top")["result"]
    guards.update(expected_result_id=camera["displayed_result_id"], expected_view_revision=camera["view_revision"])
    capture = operation("/v1/capture", **target, **guards)
    image = capture["result"]
    assert "png_base64" not in image
    png = request(image["image_route"], image=True)
    assert hashlib.sha256(png).hexdigest() == image["png_sha256"] and len(png) == image["png_bytes"]
    with Path(destination, "http-capture.png").open("xb") as output:
        output.write(png)
    assert request(capture["status_route"])["state"] == "succeeded"
    request("/v1/annotations?document_id=" + document)
    library = request(base + "/saved-views")
    added = operation(base + "/saved-views", **target, **guards,
                      expected_library_revision=library["library_revision"])["result"]
    saved_route = base + "/saved-views/" + added["added_id"]
    renamed = operation(saved_route + "/rename", **target, viewport_id=viewport,
                        expected_library_revision=added["library_revision"], name="HTTP view")["result"]
    recalled = operation(saved_route + "/recall", **target, **guards,
                         expected_library_revision=renamed["library_revision"])["result"]
    removed = operation(saved_route + "/remove", **target, viewport_id=viewport,
                        expected_library_revision=recalled["library_revision"])["result"]
    assert removed["dirty"]
    assert image["capture_policy"] == "model_only_without_guides_statistics_or_controls"
    Path(destination, "http-workflow.json").write_text(json.dumps(calls, indent=2), encoding="utf-8")
    print(json.dumps({"requests_passed": len(calls), "server_id": info["server_id"]}))


if __name__ == "__main__":
    main()
