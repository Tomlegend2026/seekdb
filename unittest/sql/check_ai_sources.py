#!/usr/bin/env python3
"""Syntax-check AI changes using the configured production compile flags."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--file", action="append", help="Check only this source (repeatable).")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    source = build_dir.parent
    commands = json.loads((build_dir / "compile_commands.json").read_text())
    template = next(entry for entry in commands if "/src/sql/" in entry["file"])
    original = template.get("arguments") or shlex.split(template["command"])
    flags = []
    position = 0
    while position < len(original):
        argument = original[position]
        if argument == "-o":
            position += 2
            continue
        if argument not in ("-c", template["file"], "-fcolor-diagnostics"):
            flags.append(argument)
        position += 1
    files = [
        "src/share/ai_service/ob_ai_service_struct.cpp",
        "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp",
        "src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp",
        "src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp",
        "src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp",
        "src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp",
        "unittest/sql/test_ai_function_regression.cpp",
    ]
    if args.file:
        unknown = set(args.file) - set(files)
        if unknown:
            parser.error(f"Unknown AI source(s): {sorted(unknown)}")
        files = args.file
    failed = []
    for name in files:
        file = source / name
        print(f"Checking {name}", flush=True)
        result = subprocess.run(
            flags + ["-fsyntax-only", "-fno-color-diagnostics", "-ferror-limit=8", str(file)],
            cwd=template["directory"],
            check=False,
            timeout=180,
        )
        if result.returncode:
            failed.append(name)
    print(f"Checked {len(files)} sources; failed: {len(failed)}", flush=True)
    for name in failed:
        print(f"FAIL {name}")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
