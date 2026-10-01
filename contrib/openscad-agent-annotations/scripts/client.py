#!/usr/bin/env python3
"""OpenSCAD annotation bridge client; intentionally has no source-edit functionality."""
import argparse
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import sys
import time
from urllib.parse import urlsplit
import uuid


def read_connection(path):
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    url = urlsplit(value["url"])
    if (value.get("product") != "OpenSCAD" or value.get("protocol") != 1 or
            url.scheme != "http" or url.hostname != "127.0.0.1" or not url.port or
            url.username or url.password or url.path not in ("", "/") or url.query or url.fragment):
        raise ValueError("Connection must identify protocol-1 OpenSCAD at numeric IPv4 loopback.")
    uuid.UUID(value["server_id"])
    return value


def request(connection, method, route, body=None):
    if not route.startswith("/v1/") or "\r" in route or "\n" in route:
        raise ValueError("Expected a /v1/ route")
    url = urlsplit(connection["url"])
    transport = http.client.HTTPConnection("127.0.0.1", url.port, timeout=10)
    headers = {"X-Agent-Annotations": "1", "X-Agent-Annotations-Server": connection["server_id"]}
    data = None if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8")
    if data is not None:
        if len(data) > 256 * 1024:
            raise ValueError("Request exceeds 256 KiB")
        headers["Content-Type"] = "application/json"
    try:
        transport.request(method, route, body=data, headers=headers)
        response = transport.getresponse()
        data = response.read(48 * 1024 * 1024 + 1)
        if len(data) > 48 * 1024 * 1024:
            raise ValueError("Response exceeds client limit")
        value = json.loads(data)
        if response.status >= 300:
            raise RuntimeError(f"HTTP {response.status}: {value.get('error', value)}")
        return value
    finally:
        transport.close()


def output(value, filename):
    value = dict(value)
    if filename:
        encoded = value.pop("png_base64", None)
        if encoded is None:
            raise ValueError("Response contains no PNG")
        png = base64.b64decode(encoded, validate=True)
        if (len(png) > 32 * 1024 * 1024 or not png.startswith(b"\x89PNG\r\n\x1a\n") or
                hashlib.sha256(png).hexdigest() != value.get("png_sha256")):
            raise ValueError("Invalid PNG/checksum")
        Path(filename).write_bytes(png)
        value["image_file"] = str(Path(filename).resolve())
    else:
        value.pop("png_base64", None)
    print(json.dumps(value, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--connection", help="Explicit discovery/connection JSON file")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("discover")
    get = sub.add_parser("get"); get.add_argument("route"); get.add_argument("--output")
    post = sub.add_parser("post"); post.add_argument("route"); post.add_argument("--body", required=True)
    post.add_argument("--output"); post.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if args.command == "discover":
        root = Path(os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
        instances = []
        for path in sorted((root / "AgentAnnotations/OpenSCAD").glob("*.json")):
            try:
                info = read_connection(path)
                live = request(info, "GET", "/v1/info")
                instances.append({"connection_file": str(path), **live})
            except (OSError, ValueError, KeyError, RuntimeError, http.client.HTTPException):
                continue
        print(json.dumps(instances, indent=2)); return
    if not args.connection:
        parser.error("--connection is required; select the intended instance explicitly")
    connection = read_connection(args.connection)
    if args.command == "get":
        value = request(connection, "GET", args.route)
        if value.get("state") == "succeeded": value = value["result"]
        output(value, args.output); return
    body = json.loads(Path(args.body).read_text(encoding="utf-8"))
    uuid.UUID(body["request_id"])
    if "server_id" in body and body["server_id"] != connection["server_id"]:
        raise ValueError("Request belongs to a different server session")
    body["server_id"] = connection["server_id"]
    print(f"Request ID: {body['request_id']} (reuse this body on retry)", file=sys.stderr)
    value = request(connection, "POST", args.route, body)
    end = time.monotonic() + max(0, args.timeout)
    while value.get("state") == "queued" and time.monotonic() < end:
        time.sleep(0.25)
        value = request(connection, "GET", "/v1/requests/" + body["request_id"])
    if value.get("state") != "succeeded":
        raise RuntimeError(json.dumps(value))
    output(value["result"], args.output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, http.client.HTTPException) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
