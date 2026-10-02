"""Native-test driver: all actions use the public CLI from a separately staged package."""
import json
from pathlib import Path
import subprocess
import sys


def main():
    connection, document, window, viewport, destination = sys.argv[1:]
    client = Path(__file__).parents[1] / "scripts/client.py"
    calls = []
    def run(*args):
        process = subprocess.run([sys.executable, str(client), "--connection", connection, *args], capture_output=True, text=True, encoding="utf-8", timeout=20)
        calls.append({"arguments": args, "exit_code": process.returncode, "stderr": process.stderr})
        if process.returncode:
            raise RuntimeError(process.stderr)
        return json.loads(process.stdout)
    target = ("--document", document, "--window", window)
    info = run("info")
    assert info["api_version"] == 1
    assert any(item["document_id"] == document for item in run("documents")["documents"])
    captured = run("source", *target, "--origin", "disk")["result"]
    snapshot = run("snapshot", captured["snapshot_id"])
    assert snapshot["origin"] == "disk" and "cube" in snapshot["text"]
    parsed = run("parse", *target, "--snapshot", captured["snapshot_id"])["result"]
    assert parsed["success"]
    syntax = run("tree", parsed["tree_id"])
    root = run("node", parsed["tree_id"], syntax["root_ids"][0])
    edges = run("children", parsed["tree_id"], root["node_id"])
    assert edges["children"]
    run("dependencies", "--document", document)
    run("diagnostics", "--document", document)
    results = run("results", "--document", document)["results"]
    model = run("tree", results[-1]["tree_id"])
    assert model["kind"] == "evaluated"
    assert run("node", model["tree_id"], model["root_ids"][0])["children"]
    status = run("status")
    view = next(v for v in status["viewports"] if v["viewport_id"] == viewport)
    guards = ("--viewport", viewport, "--expected-result", view["displayed_result_id"], "--expected-view", view["view_revision"])
    camera = run("view", *target, *guards, "--operation", "standard", "--view", "top")["result"]
    guards = ("--viewport", viewport, "--expected-result", camera["displayed_result_id"], "--expected-view", camera["view_revision"])
    image = run("capture", *target, *guards, "--output", str(Path(destination) / "client-capture.png"))
    assert image["result"]["view_revision"] == camera["view_revision"]
    assert Path(image["result"]["image_file"]).is_file()
    assert run("outcome", image["request_id"])["state"] == "succeeded"
    run("annotations", "--document", document)
    if "saved_views.write" in info["capabilities"]:
        library = run("saved-views", "--document", document)
        added = run("saved-add", *target, *guards, "--expected-library", library["library_revision"])["result"]
        renamed = run("saved-rename", added["added_id"], *target, "--viewport", viewport,
                      "--expected-library", added["library_revision"], "--name", "CLI view")["result"]
        recalled = run("saved-recall", added["added_id"], *target, *guards,
                       "--expected-library", renamed["library_revision"])["result"]
        removed = run("saved-remove", added["added_id"], *target, "--viewport", viewport,
                      "--expected-library", recalled["library_revision"])["result"]
        assert removed["dirty"]
        assert image["result"]["capture_policy"] == "model_only_without_guides_statistics_or_controls"
    Path(destination, "client-workflow.json").write_text(json.dumps(calls, indent=2), encoding="utf-8")
    print(json.dumps({"commands_passed": len(calls), "server_id": info["server_id"]}))


if __name__ == "__main__":
    main()
