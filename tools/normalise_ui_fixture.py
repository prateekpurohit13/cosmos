#!/usr/bin/env python3
"""Normalise host-dependent fields in a checked-in UI fixture.

The exporter records real wall-clock finish times and host wall durations. Those are metadata, not
identity, and they would make the fixture differ on every run. This script rewrites only those
fields (and the compiler string) so `just ui-fixture` is reproducible; it deliberately does not
round-trip the JSON, because the writer's formatting is part of what the fixture demonstrates.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

FIXED_CREATED_NS = 1792995643348122804
FIXED_COMPLETED_NS = 1792995643349177376


def replace_number(text: str, key: str, value: int, where: Path) -> str:
    pattern = re.compile(r'("' + re.escape(key) + r'"\s*:\s*)[0-9]+')
    result, count = pattern.subn(lambda match: match.group(1) + str(value), text, count=1)
    if count != 1:
        raise SystemExit(f"{where}: expected exactly one {key}")
    return result


def replace_compiler(text: str, where: Path) -> str:
    pattern = re.compile(r'("compiler"\s*:\s*)"[^"]*"')
    result, count = pattern.subn(r'\1"fixture"', text, count=1)
    if count != 1:
        raise SystemExit(f"{where}: expected exactly one compiler field")
    return result


def check_json(path: Path) -> None:
    try:
        json.loads(path.read_text())
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path}: invalid JSON: {error}") from error


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: normalise_ui_fixture.py <campaign-directory>")
    root = Path(sys.argv[1])
    manifest = root / "manifest.json"
    if not manifest.is_file():
        raise SystemExit(f"{manifest}: missing")
    text = manifest.read_text()
    text = replace_number(text, "created_unix_ns", FIXED_CREATED_NS, manifest)
    text = replace_number(text, "completed_unix_ns", FIXED_COMPLETED_NS, manifest)
    text = replace_compiler(text, manifest)
    manifest.write_text(text)
    check_json(manifest)

    for path in sorted((root / "universes").glob("*.json")):
        text = path.read_text()
        text = replace_number(text, "wall_ns", 0, path)
        path.write_text(text)
        check_json(path)

    index = root / "universes.ndjson"
    lines = index.read_text().splitlines()
    if not lines:
        raise SystemExit(f"{index}: no universe rows")
    normalised = [replace_number(line, "wall_ns", 0, index) for line in lines]
    index.write_text("\n".join(normalised) + "\n")
    for line in normalised:
        json.loads(line)

    document = json.loads(manifest.read_text())
    if not document.get("complete"):
        raise SystemExit(f"{manifest}: fixture is not a complete campaign")
    print(f"normalised {len(normalised)} universes in {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
