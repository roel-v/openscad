#!/usr/bin/env python3
"""Check UTF-8 command-line filenames, directories, and include/use lookup with real exports."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    output_root = Path(sys.argv[2]).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    # Retain distinct fixtures and logs for parallel invocations and failure diagnosis.
    root = Path(tempfile.mkdtemp(prefix="run-", dir=output_root))
    cases = [
        ("ascii", "ascii", "cube", "result"),
        ("latin", "latin", "cube-caf\u00e9-\u00f6", "result-\u00df"),
        ("chinese", "chinese", "\u6a21\u578b", "\u7ed3\u679c"),
        ("supplementary", "supplementary", "cube-\U0001f600", "result-\U0001f680"),
        ("directory", "folder caf\u00e9 \u6a21\u578b \U0001f600", "cube", "result"),
        ("combined", "combined \u6a21\u578b", "cube-\u00f6", "result-\U0001f600"),
        ("include-use", "includes", "cube", "result"),
    ]
    results = []
    reference = None
    for label, directory, input_name, output_name in cases:
        folder = root / directory
        folder.mkdir()
        source = folder / (input_name + ".scad")
        destination = folder / (output_name + ".stl")
        if label == "include-use":
            (folder / "parameters-\u00f6.scad").write_text("size = [3, 4, 5];\n", encoding="utf-8")
            (folder / "shape-\u6a21\u578b-\U0001f600.scad").write_text(
                "module shape(size) { cube(size); }\n", encoding="utf-8")
            text = "include <parameters-\u00f6.scad>\nuse <shape-\u6a21\u578b-\U0001f600.scad>\nshape(size);\n"
        else:
            text = "cube([3, 4, 5]);\n"
        source.write_text(text, encoding="utf-8")
        command = [str(executable), "--export-format", "asciistl", "-o", str(destination), str(source)]
        try:
            completed = subprocess.run(command, capture_output=True, timeout=20)
            output = completed.stdout + completed.stderr
            (folder / "output.log").write_bytes(output)
            data = destination.read_bytes() if destination.exists() else b""
            valid = completed.returncode == 0 and b"ERROR:" not in output and b"WARNING:" not in output
            valid = valid and data.startswith(b"solid ") and data.count(b"facet normal") == 12
            if label == "ascii" and valid:
                reference = data
            valid = valid and reference is not None and data == reference
            results.append({"case": label, "passed": valid, "exitCode": completed.returncode,
                            "command": command, "sha256": hashlib.sha256(data).hexdigest()})
        except subprocess.TimeoutExpired:
            results.append({"case": label, "passed": False, "error": "Timed out"})
        print(f"{label}: {'PASS' if results[-1]['passed'] else 'FAIL'}")
    (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print("Evidence: " + str(root).encode("ascii", "backslashreplace").decode("ascii"))
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
