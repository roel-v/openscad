#!/usr/bin/env python3
"""Inspect and control an explicitly selected OpenSCAD GUI through its local agent API."""
import argparse
import base64
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
from urllib.parse import urlencode, urlsplit, quote
import uuid
import zlib

MAX_RESPONSE = 48 * 1024 * 1024


class AgentError(RuntimeError):
    pass


def identifier(value):
    try:
        parsed = uuid.UUID(value)
    except (ValueError, TypeError, AttributeError) as error:
        raise ValueError("Expected a nonzero UUID") from error
    if parsed.int == 0:
        raise ValueError("Expected a nonzero UUID")
    return str(parsed)


def connection(value):
    url = urlsplit(value["url"])
    if (value.get("product") != "OpenSCAD" or value.get("protocol") != 1 or
            url.scheme != "http" or url.hostname != "127.0.0.1" or not url.port or
            url.username or url.password or url.path not in ("", "/") or url.query or url.fragment):
        raise ValueError("Expected an OpenSCAD connection at numeric IPv4 loopback")
    identifier(value["server_id"])
    return value


def request(info, method, route, body=None, timeout=10):
    connection(info)
    parsed = urlsplit(route)
    if (not route.startswith("/v2/") or parsed.scheme or parsed.netloc or parsed.fragment or
            any(ord(char) < 32 for char in route) or ".." in parsed.path.split("/")):
        raise ValueError("Expected a local /v2/ route or returned cursor")
    data = None if body is None else json.dumps(body, ensure_ascii=False, allow_nan=False).encode("utf-8")
    if data is not None and len(data) > 256 * 1024:
        raise ValueError("Request exceeds 256 KiB")
    headers = {"X-OpenSCAD-Agent": "1", "X-OpenSCAD-Agent-Server": info["server_id"]}
    if data is not None:
        headers["Content-Type"] = "application/json"
    transport = http.client.HTTPConnection("127.0.0.1", urlsplit(info["url"]).port, timeout=timeout)
    try:
        transport.request(method, route, body=data, headers=headers)
        response = transport.getresponse()
        data = response.read(MAX_RESPONSE + 1)
        if len(data) > MAX_RESPONSE:
            raise AgentError("Response exceeds 48 MiB")
        if response.getheader("Content-Type", "").split(";")[0] != "application/json":
            raise AgentError("Expected JSON; redirects are never followed")
        value = json.loads(data)
        if not isinstance(value, dict):
            raise AgentError("Expected a JSON object")
        if response.status != 200 and response.status != 202:
            raise AgentError(f"HTTP {response.status}: {json.dumps(value, ensure_ascii=False)}")
        return value
    finally:
        transport.close()


def verify(info):
    live = request(info, "GET", "/v2/info", timeout=2)
    if (live.get("server_id") != info["server_id"] or live.get("api_version") != 2 or
            live.get("product") != "OpenSCAD" or live.get("access_mode") != "agent"):
        raise AgentError("Instance identity/version changed; select it explicitly again")
    return live


def discover():
    if sys.platform == "win32":
        root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    elif sys.platform == "darwin":
        root = Path.home() / "Library/Application Support"
    else:
        root = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    instances = []
    for path in sorted((root / "AgentAnnotations/OpenSCAD").glob("*.json")):
        try:
            info = connection(json.loads(path.read_text(encoding="utf-8-sig")))
            live = verify(info)
            instances.append({**live, "connection_file": str(path.resolve())})
        except (OSError, ValueError, KeyError, AgentError, http.client.HTTPException):
            continue
    return {"instances": instances}


def operation(info, route, body, timeout=60):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Timeout must be positive and finite")
    body = dict(body)
    if body.get("server_id", info["server_id"]) != info["server_id"]:
        raise ValueError("Request belongs to a different server session")
    body["server_id"] = info["server_id"]
    body["request_id"] = identifier(body.get("request_id") or str(uuid.uuid4()))
    request_id = body["request_id"]
    print(f"Server {info['server_id']}; request {request_id}. Recover with outcome; do not submit a new ID after uncertainty.", file=sys.stderr)
    end = time.monotonic() + timeout
    value = request(info, "POST", route, body, timeout=min(10, timeout))
    delay = 0.1
    while True:
        if value.get("request_id") != request_id or value.get("server_id") != info["server_id"]:
            raise AgentError("Receipt identity mismatch")
        state = value.get("state")
        if state == "succeeded":
            if not isinstance(value.get("result"), dict):
                raise AgentError("Malformed successful receipt")
            return value
        if state not in ("queued", "running"):
            raise AgentError(json.dumps(value, ensure_ascii=False))
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise AgentError(f"Outcome uncertain for {request_id}; poll this same server/request. No cancellation or retry was attempted.")
        time.sleep(min(delay, remaining))
        delay = min(1.0, delay * 1.5)
        remaining = end - time.monotonic()
        if remaining <= 0:
            continue
        value = request(info, "GET", "/v2/requests/" + request_id, timeout=min(10, remaining))


def png_bytes(value):
    png = base64.b64decode(value["png_base64"], validate=True)
    if (len(png) > 32 * 1024 * 1024 or not png.startswith(b"\x89PNG\r\n\x1a\n") or
            hashlib.sha256(png).hexdigest() != value.get("png_sha256")):
        raise ValueError("Invalid PNG/checksum")
    position, dimensions, ended = 8, None, False
    while position + 12 <= len(png):
        length = struct.unpack_from(">I", png, position)[0]
        kind = png[position + 4:position + 8]
        end = position + 12 + length
        if end > len(png):
            raise ValueError("Truncated PNG chunk")
        payload = png[position + 8:position + 8 + length]
        crc = struct.unpack_from(">I", png, position + 8 + length)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != crc:
            raise ValueError("Invalid PNG chunk checksum")
        if position == 8:
            if kind != b"IHDR" or length != 13:
                raise ValueError("Missing PNG IHDR")
            dimensions = struct.unpack_from(">II", payload)
        position = end
        if kind == b"IEND":
            ended = length == 0 and position == len(png)
            break
    if (not ended or dimensions != (value.get("width"), value.get("height")) or
            not dimensions or min(dimensions) <= 0 or dimensions[0] * dimensions[1] > 64000000):
        raise ValueError("Invalid PNG dimensions or end chunk")
    return png


def output(value, filename=None):
    value = dict(value)
    image = dict(value["result"]) if isinstance(value.get("result"), dict) else value
    if filename:
        png = png_bytes(image)
        path = Path(filename).resolve()
        # Exclusive create prevents silently replacing existing evidence.
        with path.open("xb") as stream:
            stream.write(png)
        image["image_file"] = str(path)
    image.pop("png_base64", None)
    if isinstance(value.get("result"), dict):
        value["result"] = image
    print(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False))


def parser():
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--connection", help="Explicit discovery/Copy connection info JSON file")
    sub = root.add_subparsers(dest="command", required=True)
    for name in ("instances", "info", "status", "documents", "viewports"):
        sub.add_parser(name)
    for name in ("dependencies", "diagnostics", "attempts", "results", "parameters"):
        p = sub.add_parser(name); p.add_argument("--document", required=True, type=identifier)
    for name in ("tree", "node", "children"):
        p = sub.add_parser(name); p.add_argument("tree", type=identifier)
        if name != "tree":
            p.add_argument("node")
    p = sub.add_parser("annotations"); p.add_argument("--document", required=True, type=identifier); p.add_argument("--query", default="")
    p = sub.add_parser("annotation"); p.add_argument("id", type=identifier); p.add_argument("--document", required=True, type=identifier); p.add_argument("--output")
    p = sub.add_parser("snapshot"); p.add_argument("id", type=identifier)
    p = sub.add_parser("outcome"); p.add_argument("id", type=identifier); p.add_argument("--output")
    p = sub.add_parser("get"); p.add_argument("route"); p.add_argument("--output")
    p = sub.add_parser("saved-views"); p.add_argument("--document", required=True, type=identifier)
    for name in ("saved-add", "saved-rename", "saved-remove", "saved-recall"):
        p = sub.add_parser(name)
        p.add_argument("--document", required=True, type=identifier)
        p.add_argument("--window", required=True, type=identifier)
        p.add_argument("--viewport", required=True, type=identifier)
        p.add_argument("--expected-library", required=True)
        p.add_argument("--request-id", type=identifier)
        p.add_argument("--timeout", type=float, default=60)
        if name in ("saved-add", "saved-recall"):
            p.add_argument("--expected-result", required=True)
            p.add_argument("--expected-view", required=True)
        if name != "saved-add":
            p.add_argument("id", type=identifier)
        if name == "saved-rename":
            p.add_argument("--name", required=True)
    for name in ("source", "parse", "view", "restore", "capture", "cancel", "post"):
        p = sub.add_parser(name)
        p.add_argument("--request-id", type=identifier)
        p.add_argument("--timeout", type=float, default=60)
        if name not in ("cancel", "post"):
            p.add_argument("--document", required=True, type=identifier)
            p.add_argument("--window", required=True, type=identifier)
        if name in ("view", "restore", "capture"):
            p.add_argument("--viewport", required=True, type=identifier)
            p.add_argument("--expected-result", required=True)
            p.add_argument("--expected-view", required=True)
        if name == "source":
            p.add_argument("--origin", choices=("disk", "editor"), required=True)
            p.add_argument("--expected-revision")
            p.add_argument("--expected-hash")
            p.add_argument("--dependency", type=identifier)
        elif name == "parse":
            p.add_argument("--snapshot", required=True, type=identifier)
        elif name == "view":
            p.add_argument("--operation", choices=("set", "standard", "projection", "fit"), required=True)
            p.add_argument("--camera", help="JSON file containing a complete camera")
            p.add_argument("--view", choices=("top", "bottom", "left", "right", "front", "back", "diagonal"))
            p.add_argument("--projection", choices=("orthographic", "perspective"))
        elif name in ("restore", "cancel"):
            p.add_argument("id", type=identifier)
        elif name == "capture":
            p.add_argument("--output", required=True)
        elif name == "post":
            p.add_argument("route"); p.add_argument("--body", required=True); p.add_argument("--output")
    return root


def run(args):
    if args.command == "instances":
        return discover()
    if not args.connection:
        raise ValueError("--connection is required; choose the intended instance explicitly")
    info = connection(json.loads(Path(args.connection).read_text(encoding="utf-8-sig")))
    live = verify(info)
    command = args.command
    if command == "saved-views" or command.startswith("saved-"):
        reading = command == "saved-views"
        capability = "saved_views.read" if reading else "saved_views.write"
        if capability not in live.get("capabilities", []):
            raise AgentError(f"Unsupported capability: {capability}")
        route = f"/v2/documents/{args.document}/saved-views"
        if reading:
            return request(info, "GET", route)
        body = {"document_id": args.document, "window_id": args.window, "viewport_id": args.viewport,
                "expected_library_revision": args.expected_library, "request_id": args.request_id or str(uuid.uuid4())}
        if command in ("saved-add", "saved-recall"):
            body.update(expected_result_id=args.expected_result, expected_view_revision=args.expected_view)
        if command != "saved-add":
            route += f"/{args.id}/{command[6:]}"
        if command == "saved-rename":
            body["name"] = args.name
        return operation(info, route, body, args.timeout)
    if command == "info":
        return live
    if command in ("status", "documents", "viewports"):
        return request(info, "GET", "/v2/" + command)
    if command in ("dependencies", "diagnostics", "attempts", "results", "parameters"):
        capability = command + ".read"
        if capability not in live.get("capabilities", []):
            raise AgentError(f"Unsupported capability: {capability}")
        return request(info, "GET", f"/v2/documents/{args.document}/{command}")
    if command in ("tree", "node", "children"):
        route = "/v2/trees/" + args.tree
        if command != "tree":
            route += "/nodes/" + quote(args.node, safe="")
        if command == "children":
            route += "/children"
        return request(info, "GET", route)
    if command in ("annotations", "annotation"):
        route = "/v2/annotations" + ("/" + args.id if command == "annotation" else "")
        return request(info, "GET", route + "?" + urlencode({"document_id": args.document, "q": getattr(args, "query", "")}))
    if command in ("snapshot", "outcome", "get"):
        route = args.route if command == "get" else ("/v2/source-snapshots/" if command == "snapshot" else "/v2/requests/") + args.id
        return request(info, "GET", route)
    body = {"request_id": args.request_id or str(uuid.uuid4())}
    if command == "post":
        body = json.loads(Path(args.body).read_text(encoding="utf-8-sig"))
        if not isinstance(body, dict):
            raise ValueError("Body must be a JSON object")
        if args.request_id:
            if body.get("request_id", args.request_id) != args.request_id:
                raise ValueError("Conflicting request IDs")
            body["request_id"] = args.request_id
        return operation(info, args.route, body, args.timeout)
    if command == "cancel":
        return operation(info, f"/v2/requests/{args.id}/cancel", body, args.timeout)
    body.update(document_id=args.document, window_id=args.window)
    if command == "source":
        body["origin"] = args.origin
        if args.origin == "editor" and not args.expected_revision:
            raise ValueError("Editor origin requires --expected-revision")
        for key, value in (("expected_source_revision", args.expected_revision), ("expected_source_sha256", args.expected_hash), ("dependency_id", args.dependency)):
            if value is not None:
                body[key] = value
        route, capability = f"/v2/documents/{args.document}/source-snapshots", "source.read"
    elif command == "parse":
        body["snapshot_id"] = args.snapshot
        route, capability = f"/v2/documents/{args.document}/parse", "syntax.parse"
    else:
        body.update(viewport_id=args.viewport, expected_result_id=args.expected_result, expected_view_revision=args.expected_view)
        if command == "capture":
            route, capability = "/v2/capture", "viewport.capture"
        elif command == "restore":
            route, capability = f"/v2/annotations/{args.id}/restore", "camera.restore"
        else:
            route, capability = f"/v2/viewports/{args.viewport}/view", "camera.set"
            body["operation"] = args.operation
            if args.operation == "set":
                if not args.camera:
                    raise ValueError("set requires --camera JSON file")
                body["camera"] = json.loads(Path(args.camera).read_text(encoding="utf-8-sig"))
            if args.operation == "standard":
                if not args.view:
                    raise ValueError("standard requires --view")
                body["view"] = args.view
            if args.operation == "projection":
                if not args.projection:
                    raise ValueError("projection requires --projection")
                body["projection"] = args.projection
    if capability not in live.get("capabilities", []):
        raise AgentError(f"Unsupported capability: {capability}")
    return operation(info, route, body, args.timeout)


def main():
    # Redirected Windows streams otherwise use the legacy ANSI code page.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    args = parser().parse_args()
    output(run(args), getattr(args, "output", None))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, AgentError, http.client.HTTPException) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
