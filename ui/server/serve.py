#!/usr/bin/env python3
"""Local, read-only artifact server for the Cosmos UI.

The simulation binary never hosts HTTP (docs/ui-design.md D4). This process reads one
campaign artifact root written by ``include/cosmos/export.hpp`` and serves:

* ``ui/web`` as the static application,
* a small JSON API over the schema-v1 artifact layout, including a read-only canonical
  trace decoder for the timeline,
* SPA fallback for the client routes (``/campaigns/...``).

It binds to loopback by default and has no authentication. Point it at an artifact
root that you trust; it is a local engineering tool, not a shared service.
"""

from __future__ import annotations

import argparse
import functools
import ipaddress
import json
import math
import re
import struct
import sys
import urllib.parse
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
MAX_SEED = (1 << 64) - 1
CAMPAIGN_ID_RE = re.compile(r"^[A-Za-z0-9._-]+$")
SEED_DEC_RE = re.compile(r"^[0-9]+$")
SEED_HEX_RE = re.compile(r"^0[xX][0-9a-fA-F]+$")
LOOPBACK_HOSTS = {"localhost", "127.0.0.1", "::1"}

# Canonical trace codec (include/cosmos/trace.hpp). The server decodes the exact bytes that
# trace_hash() covers; it never re-encodes or rewrites them.
TRACE_SOURCE = 1
TRACE_HEADER = struct.Struct("<BQQ")
TRACE_ENTRY = struct.Struct("<QQqQqBBB")
TRACE_HEADER_BYTES = TRACE_HEADER.size
TRACE_ENTRY_BYTES = TRACE_ENTRY.size
# The browser keeps every entry in memory and redraws the canvas on resize/theme changes.
# Refuse an artifact larger than the view's documented ceiling rather than freezing the tab.
MAX_TRACE_ENTRIES = 100_000
MAX_TRACE_BYTES = TRACE_HEADER_BYTES + MAX_TRACE_ENTRIES * TRACE_ENTRY_BYTES
MAX_SAFE_JS_INTEGER = (1 << 53) - 1
assert TRACE_HEADER_BYTES == 17
assert TRACE_ENTRY_BYTES == 43

SITE_NAMES = {
    1: "malloc",
    2: "calloc",
    3: "realloc",
    4: "open",
    5: "read",
    6: "write",
    7: "fsync",
    8: "connect",
    9: "accept",
    10: "send",
    11: "recv",
    12: "clock_gettime",
    13: "nanosleep",
    14: "getrandom",
    1000: "crash_node",
    1001: "partition",
    1002: "pause_node",
    1003: "torn_write",
}
SITE_CLASSES = {
    1: "Memory",
    2: "Memory",
    3: "Memory",
    4: "Storage",
    5: "Storage",
    6: "Storage",
    7: "Storage",
    8: "Network",
    9: "Network",
    10: "Network",
    11: "Network",
    12: "Clock",
    13: "Clock",
    14: "Random",
    1000: "Process",
    1001: "Process",
    1002: "Process",
    1003: "Storage",
}
OUTCOME_NAMES = {
    0: "None",
    1: "OutOfMemory",
    2: "OpenEio",
    3: "ReadEio",
    4: "WriteEio",
    5: "ShortWrite",
    6: "NoSpace",
    7: "FsyncEio",
    8: "ConnRefused",
    9: "ConnReset",
    10: "PeerClose",
    11: "ShortSend",
    12: "PacketDrop",
    13: "PacketDelay",
    14: "PacketReorder",
    15: "PacketCorrupt",
    16: "ClockStep",
    17: "SleepInterrupted",
    18: "RandomEagain",
    19: "TornWrite",
}
STATUS_NAMES = {0: "FIRED", 1: "SUPPRESSED", 2: "SKIPPED"}
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211

SECURITY_HEADERS = {
    "Cache-Control": "no-store",
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "no-referrer",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Permissions-Policy": "camera=(), geolocation=(), microphone=()",
    "Content-Security-Policy": (
        "default-src 'self'; "
        "script-src 'self'; "
        "style-src 'self'; "
        "img-src 'self' data:; "
        "font-src 'self'; "
        "connect-src 'self'; "
        "base-uri 'none'; "
        "form-action 'self'; "
        "frame-ancestors 'none'"
    ),
}


class ArtifactError(Exception):
    """An expected artifact/API failure that becomes a JSON error response."""

    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = int(status)
        self.code = code
        self.message = message


def _parse_json_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"non-finite JSON number {text!r}")
    return value


def _reject_json_constant(text: str) -> None:
    raise ValueError(f"non-finite JSON constant {text!r}")


def _sanitize_text(text: str) -> str:
    """Replace lone surrogates with U+FFFD so a response always encodes as UTF-8."""
    return "".join("\ufffd" if 0xD800 <= ord(ch) <= 0xDFFF else ch for ch in text)


def _sanitize_json(value: Any) -> Any:
    if isinstance(value, str):
        return _sanitize_text(value)
    if isinstance(value, list):
        return [_sanitize_json(item) for item in value]
    if isinstance(value, dict):
        return {
            _sanitize_text(str(key)): _sanitize_json(item) for key, item in value.items()
        }
    return value


def is_loopback(host: str) -> bool:
    return host in LOOPBACK_HOSTS


def safe_campaign_id(raw: str) -> str:
    if not isinstance(raw, str) or not raw:
        raise ArtifactError(HTTPStatus.BAD_REQUEST, "invalid_campaign_id", "campaign id is empty")
    if len(raw) > 200:
        raise ArtifactError(
            HTTPStatus.BAD_REQUEST, "invalid_campaign_id", "campaign id is too long"
        )
    if raw in (".", "..") or not CAMPAIGN_ID_RE.fullmatch(raw):
        raise ArtifactError(
            HTTPStatus.BAD_REQUEST,
            "invalid_campaign_id",
            "campaign id must be a single path component ([A-Za-z0-9._-]+)",
        )
    return raw


def parse_seed(raw: str) -> int:
    if not isinstance(raw, str) or not raw:
        raise ArtifactError(HTTPStatus.BAD_REQUEST, "invalid_seed", "seed is empty")
    try:
        if SEED_HEX_RE.fullmatch(raw):
            value = int(raw, 16)
        elif SEED_DEC_RE.fullmatch(raw):
            value = int(raw, 10)
        else:
            raise ArtifactError(
                HTTPStatus.BAD_REQUEST,
                "invalid_seed",
                "seed must be a decimal integer or a 0x-prefixed hexadecimal integer",
            )
    except ValueError as exc:
        raise ArtifactError(
            HTTPStatus.BAD_REQUEST,
            "invalid_seed",
            "seed is too long to parse as a 64-bit integer",
        ) from exc
    if value > MAX_SEED:
        raise ArtifactError(HTTPStatus.BAD_REQUEST, "invalid_seed", "seed is wider than 64 bits")
    return value


def _fnv1a_64(data: bytes) -> int:
    digest = FNV_OFFSET
    for byte in data:
        digest ^= byte
        digest = (digest * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return digest


def _trace_bad_value(where: str, message: str) -> ArtifactError:
    return ArtifactError(
        HTTPStatus.INTERNAL_SERVER_ERROR,
        "artifact_trace_malformed",
        f"{where}: {message}",
    )


def decode_trace_bytes(data: bytes, where: str) -> dict[str, Any]:
    """Decode canonical trace bytes into the frozen TraceEntry fields.

    Validation mirrors decode_trace() in include/cosmos/trace.hpp: source byte, count, universe
    seed, then fixed-width little-endian entries with known site/outcome/status tokens. The
    declared trace_hash is deliberately not checked here; the caller reports a mismatch without
    hiding decodable evidence.
    """
    if len(data) < TRACE_HEADER_BYTES:
        raise _trace_bad_value(where, "header is truncated")
    source, count, seed = TRACE_HEADER.unpack_from(data, 0)
    if source != TRACE_SOURCE:
        raise _trace_bad_value(where, f"unsupported trace source {source}")
    if count > MAX_TRACE_ENTRIES:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_trace_too_large",
            f"{where}: trace declares {count} entries; this UI supports at most "
            f"{MAX_TRACE_ENTRIES}",
        )
    expected = count * TRACE_ENTRY_BYTES
    remaining = len(data) - TRACE_HEADER_BYTES
    if remaining < expected:
        raise _trace_bad_value(
            where,
            f"entry payload is truncated (declared {count} entries, found {remaining} bytes)",
        )
    if remaining > expected:
        raise _trace_bad_value(
            where,
            f"trailing bytes after {count} entries ({remaining - expected} extra)",
        )

    entries: list[dict[str, Any]] = []
    offset = TRACE_HEADER_BYTES
    previous_seq = 0
    for index in range(count):
        (
            seq,
            eligible_index,
            at_ns,
            site_id,
            amount_ns,
            outcome_id,
            drew,
            status_id,
        ) = TRACE_ENTRY.unpack_from(data, offset)
        offset += TRACE_ENTRY_BYTES
        if seq <= previous_seq:
            raise _trace_bad_value(
                where, f"entry {index} has non-increasing seq {seq} after {previous_seq}"
            )
        previous_seq = seq
        site = SITE_NAMES.get(site_id)
        if site is None:
            raise _trace_bad_value(where, f"entry {index} has unknown site id {site_id}")
        outcome = OUTCOME_NAMES.get(outcome_id)
        if outcome is None:
            raise _trace_bad_value(where, f"entry {index} has unknown outcome id {outcome_id}")
        if drew > 1:
            raise _trace_bad_value(where, f"entry {index} has invalid drew flag {drew}")
        if (
            seq > MAX_SAFE_JS_INTEGER
            or eligible_index > MAX_SAFE_JS_INTEGER
            or abs(at_ns) > MAX_SAFE_JS_INTEGER
            or abs(amount_ns) > MAX_SAFE_JS_INTEGER
        ):
            raise _trace_bad_value(
                where,
                f"entry {index} has a field outside the safe-integer display range",
            )
        status = STATUS_NAMES.get(status_id)
        if status is None:
            raise _trace_bad_value(where, f"entry {index} has unknown status id {status_id}")
        entries.append(
            {
                "seq": seq,
                "eligible_index": eligible_index,
                "at_ns": at_ns,
                "site": site,
                "site_id": site_id,
                "fault_class": SITE_CLASSES[site_id],
                "amount_ns": amount_ns,
                "outcome": outcome,
                "outcome_id": outcome_id,
                "drew": drew == 1,
                "status": status,
                "status_id": status_id,
            }
        )

    return {
        "source": source,
        "seed": str(seed),
        "seed_hex": f"0x{seed:016x}",
        "count": count,
        "size_bytes": len(data),
        "computed_hash": f"0x{_fnv1a_64(data):016x}",
        "entries": entries,
    }


def _artifact_file(root: Path, *parts: str) -> Path:
    """Resolve a fixed artifact path and refuse anything that leaves the campaign root."""
    candidate = root.joinpath(*parts).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_path_escape",
            "artifact path escapes its campaign directory",
        ) from exc
    return candidate


def _read_json_object(path: Path, description: str) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(
                handle,
                parse_float=_parse_json_float,
                parse_constant=_reject_json_constant,
            )
    except FileNotFoundError as exc:
        raise ArtifactError(
            HTTPStatus.NOT_FOUND, "artifact_missing", f"{description} not found"
        ) from exc
    except UnicodeDecodeError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_not_utf8",
            f"{description} is not valid UTF-8",
        ) from exc
    except json.JSONDecodeError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{description} is not valid JSON (line {exc.lineno}: {exc.msg})",
        ) from exc
    except ValueError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{description} contains an invalid value: {exc}",
        ) from exc
    except RecursionError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_too_deep",
            f"{description} is nested too deeply",
        ) from exc
    except OSError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_unreadable",
            f"{description} could not be read: {exc.strerror or exc}",
        ) from exc
    if not isinstance(value, dict):
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{description} must be a JSON object",
        )
    version = value.get("schema_version")
    if type(version) is not int or version != SCHEMA_VERSION:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "unsupported_schema",
            f"{description} has schema_version {version!r}; this server understands "
            f"{SCHEMA_VERSION}",
        )
    return value


def _campaign_dir(root: Path, campaign_id: str) -> Path:
    campaign_id = safe_campaign_id(campaign_id)
    candidate = (root / campaign_id).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise ArtifactError(
            HTTPStatus.BAD_REQUEST,
            "invalid_campaign_id",
            "campaign id resolves outside the artifact root",
        ) from exc
    if not candidate.is_dir():
        raise ArtifactError(
            HTTPStatus.NOT_FOUND, "campaign_not_found", f"campaign {campaign_id!r} not found"
        )
    return candidate


def _read_manifest(campaign_dir: Path, campaign_id: str) -> dict[str, Any]:
    manifest = _read_json_object(
        _artifact_file(campaign_dir, "manifest.json"), "manifest.json"
    )
    if manifest.get("campaign_id") != campaign_id:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "campaign_identity_mismatch",
            "manifest.json campaign_id does not match its directory",
        )
    return manifest


def _read_campaign(campaign_dir: Path, campaign_id: str) -> dict[str, Any]:
    campaign = _read_json_object(
        _artifact_file(campaign_dir, "campaign.json"), "campaign.json"
    )
    if campaign.get("campaign_id") != campaign_id:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "campaign_identity_mismatch",
            "campaign.json campaign_id does not match its directory",
        )
    return campaign


def _read_campaign_required(campaign_dir: Path, campaign_id: str) -> dict[str, Any]:
    try:
        return _read_campaign(campaign_dir, campaign_id)
    except ArtifactError as exc:
        if exc.code == "artifact_missing":
            raise ArtifactError(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                "artifact_incomplete",
                "campaign.json is missing; the artifact is incomplete",
            ) from exc
        raise


def _validate_universe_row(row: Any, line_number: int) -> dict[str, Any]:
    where = f"universes.ndjson line {line_number}"
    if not isinstance(row, dict):
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR, "artifact_malformed", f"{where} is not an object"
        )
    if type(row.get("schema_version")) is not int or row.get("schema_version") != SCHEMA_VERSION:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "unsupported_schema",
            f"{where} has an unsupported schema_version",
        )
    index = row.get("index")
    if not isinstance(index, int) or isinstance(index, bool) or index < 0:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{where} has an invalid universe index",
        )
    seed = row.get("seed")
    if not isinstance(seed, str) or not SEED_DEC_RE.fullmatch(seed):
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{where} seed must be a decimal string",
        )
    try:
        seed_value = int(seed)
    except ValueError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{where} seed is not a representable integer",
        ) from exc
    if seed_value > MAX_SEED:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_malformed",
            f"{where} seed is wider than 64 bits",
        )
    return row


def _read_universe_rows(campaign_dir: Path) -> list[dict[str, Any]]:
    path = _artifact_file(campaign_dir, "universes.ndjson")
    rows: list[dict[str, Any]] = []
    seen: set[int] = set()
    try:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                if not line.strip():
                    continue
                try:
                    row = json.loads(
                        line,
                        parse_float=_parse_json_float,
                        parse_constant=_reject_json_constant,
                    )
                except json.JSONDecodeError as exc:
                    raise ArtifactError(
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "artifact_malformed",
                        f"universes.ndjson line {line_number} is not valid JSON "
                        f"({exc.msg})",
                    ) from exc
                except ValueError as exc:
                    raise ArtifactError(
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "artifact_malformed",
                        f"universes.ndjson line {line_number} contains an invalid value: "
                        f"{exc}",
                    ) from exc
                except RecursionError as exc:
                    raise ArtifactError(
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "artifact_too_deep",
                        f"universes.ndjson line {line_number} is nested too deeply",
                    ) from exc
                row = _validate_universe_row(row, line_number)
                if row["index"] in seen:
                    raise ArtifactError(
                        HTTPStatus.INTERNAL_SERVER_ERROR,
                        "artifact_malformed",
                        f"universes.ndjson repeats index {row['index']}",
                    )
                seen.add(row["index"])
                rows.append(row)
    except FileNotFoundError as exc:
        raise ArtifactError(
            HTTPStatus.NOT_FOUND, "artifact_missing", "universes.ndjson not found"
        ) from exc
    except UnicodeDecodeError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_not_utf8",
            "universes.ndjson is not valid UTF-8",
        ) from exc
    except OSError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_unreadable",
            f"universes.ndjson could not be read: {exc.strerror or exc}",
        ) from exc
    # The writer already emits ascending indices. Sorting here keeps a hand-edited
    # artifact from making the UI order depend on file order (D12).
    rows.sort(key=lambda item: item["index"])
    return rows


def _read_universe_rows_required(campaign_dir: Path) -> list[dict[str, Any]]:
    try:
        return _read_universe_rows(campaign_dir)
    except ArtifactError as exc:
        if exc.code == "artifact_missing":
            raise ArtifactError(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                "artifact_incomplete",
                "universes.ndjson is missing; the artifact is incomplete",
            ) from exc
        raise


def _catalog(root: Path) -> dict[str, Any]:
    campaigns: list[dict[str, Any]] = []
    try:
        entries = sorted(root.iterdir(), key=lambda entry: entry.name)
    except OSError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_unreadable",
            f"artifact root could not be read: {exc.strerror or exc}",
        ) from exc
    for entry in entries:
        if not entry.is_dir() or not CAMPAIGN_ID_RE.fullmatch(entry.name):
            continue
        campaign_id = entry.name
        try:
            campaign_dir = _campaign_dir(root, campaign_id)
            manifest = _read_manifest(campaign_dir, campaign_id)
        except ArtifactError as exc:
            campaigns.append(
                {
                    "campaign_id": campaign_id,
                    "readable": False,
                    "catalog_error": exc.message,
                }
            )
            continue
        campaigns.append(
            {
                "campaign_id": campaign_id,
                "readable": True,
                "name": manifest.get("name", ""),
                "program": manifest.get("program", ""),
                "complete": manifest.get("complete") is True,
                "created_unix_ns": manifest.get("created_unix_ns"),
                "completed_unix_ns": manifest.get("completed_unix_ns"),
                "error": manifest.get("error", ""),
                "build": manifest.get("build", {}),
                "config": manifest.get("config", {}),
            }
        )
    return {"schema_version": SCHEMA_VERSION, "campaigns": campaigns}


def _campaign_payload(root: Path, campaign_id: str) -> dict[str, Any]:
    campaign_dir = _campaign_dir(root, campaign_id)
    manifest = _read_manifest(campaign_dir, campaign_id)
    campaign = _read_campaign_required(campaign_dir, campaign_id)
    rows = _read_universe_rows_required(campaign_dir)
    return {
        "schema_version": SCHEMA_VERSION,
        "campaign_id": campaign_id,
        "manifest": manifest,
        "campaign": campaign,
        "universes": rows,
    }


def _universes_payload(root: Path, campaign_id: str) -> dict[str, Any]:
    campaign_dir = _campaign_dir(root, campaign_id)
    manifest = _read_manifest(campaign_dir, campaign_id)
    rows = _read_universe_rows_required(campaign_dir)
    return {
        "schema_version": SCHEMA_VERSION,
        "campaign_id": campaign_id,
        "manifest": manifest,
        "universes": rows,
    }


def _universe_payload(root: Path, campaign_id: str, seed_text: str) -> dict[str, Any]:
    campaign_dir = _campaign_dir(root, campaign_id)
    manifest = _read_manifest(campaign_dir, campaign_id)
    campaign_doc = _read_campaign_required(campaign_dir, campaign_id)
    campaign = {
        "campaign_id": campaign_doc.get("campaign_id"),
        "name": campaign_doc.get("name", ""),
        "runs": campaign_doc.get("runs"),
        "findings": [
            {"id": item.get("id"), "seeds": item.get("seeds", [])}
            for item in campaign_doc.get("findings", [])
            if isinstance(item, dict)
        ],
    }
    row = _find_universe_row(campaign_dir, seed_text)
    detail = _read_universe_detail(campaign_dir, row)
    return {
        "schema_version": SCHEMA_VERSION,
        "campaign_id": campaign_id,
        "manifest": manifest,
        "campaign": campaign,
        "universe": detail,
    }


def _find_universe_row(campaign_dir: Path, seed_text: str) -> dict[str, Any]:
    seed = parse_seed(seed_text)
    rows = _read_universe_rows_required(campaign_dir)
    row = next((item for item in rows if int(item["seed"]) == seed), None)
    if row is None:
        raise ArtifactError(
            HTTPStatus.NOT_FOUND,
            "universe_not_found",
            f"universe with seed {seed_text!r} not found",
        )
    return row


def _read_universe_detail(campaign_dir: Path, row: dict[str, Any]) -> dict[str, Any]:
    detail_path = _artifact_file(campaign_dir, "universes", f"{row['index']}.json")
    detail = _read_json_object(detail_path, f"universes/{row['index']}.json")
    try:
        same_seed = int(detail.get("seed")) == int(row["seed"])
    except (TypeError, ValueError):
        same_seed = False
    if detail.get("index") != row["index"] or not same_seed:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "universe_identity_mismatch",
            f"universes/{row['index']}.json does not match its index row",
        )
    return detail


def _universe_summary(detail: dict[str, Any]) -> dict[str, Any]:
    """The identity fields a timeline needs, without copying the whole universe document."""
    return {
        "index": detail.get("index"),
        "seed": detail.get("seed"),
        "seed_hex": detail.get("seed_hex", ""),
        "status": detail.get("status"),
        "virtual_end_ns": detail.get("virtual_end_ns"),
        "checks": detail.get("checks", []),
        "trace": detail.get("trace", {}),
    }


def _trace_payload(root: Path, campaign_id: str, seed_text: str) -> dict[str, Any]:
    campaign_dir = _campaign_dir(root, campaign_id)
    manifest = _read_manifest(campaign_dir, campaign_id)
    row = _find_universe_row(campaign_dir, seed_text)
    detail = _read_universe_detail(campaign_dir, row)
    meta = detail.get("trace")
    if not isinstance(meta, dict):
        meta = {}

    base: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "campaign_id": campaign_id,
        "manifest": manifest,
        "universe": _universe_summary(detail),
        "trace": {
            "present": False,
            "traced": bool(meta.get("traced")),
            "missing": False,
            "path": meta.get("path") if isinstance(meta.get("path"), str) else "",
            "capacity": meta.get("capacity"),
            "dropped": meta.get("dropped"),
            "declared_hash": meta.get("hash"),
            "entries": [],
        },
    }
    if not meta.get("present"):
        return base

    expected_relative = f"traces/{row['index']}.bin"
    declared_relative = meta.get("path")
    if declared_relative != expected_relative:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "trace_path_mismatch",
            f"trace path {declared_relative!r} does not match {expected_relative!r}",
        )
    trace_path = _artifact_file(campaign_dir, "traces", f"{row['index']}.bin")
    try:
        file_size = trace_path.stat().st_size
    except FileNotFoundError:
        base["trace"].update(
            {
                "missing": True,
                "error": "trace_missing",
                "message": "trace metadata says the file is present, but it is not on disk",
            }
        )
        return base
    except OSError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_unreadable",
            f"trace file could not be read: {exc.strerror or exc}",
        ) from exc
    if file_size > MAX_TRACE_BYTES:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_trace_too_large",
            f"{expected_relative}: {file_size} bytes exceeds the {MAX_TRACE_BYTES}-byte "
            "timeline limit",
        )
    try:
        data = trace_path.read_bytes()
    except OSError as exc:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_unreadable",
            f"trace file could not be read: {exc.strerror or exc}",
        ) from exc
    if len(data) > MAX_TRACE_BYTES:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "artifact_trace_too_large",
            f"{expected_relative}: {len(data)} bytes exceeds the {MAX_TRACE_BYTES}-byte "
            "timeline limit",
        )

    decoded = decode_trace_bytes(data, expected_relative)
    declared_hash = meta.get("hash")
    hash_matches = None
    if isinstance(declared_hash, str) and declared_hash:
        hash_matches = declared_hash.lower() == decoded["computed_hash"]
    base["trace"].update(
        {
            "present": True,
            "missing": False,
            "path": expected_relative,
            "source": decoded["source"],
            "seed": decoded["seed"],
            "seed_hex": decoded["seed_hex"],
            "seed_matches": int(decoded["seed"]) == int(row["seed"]),
            "count": decoded["count"],
            "size_bytes": decoded["size_bytes"],
            "computed_hash": decoded["computed_hash"],
            "hash_matches": hash_matches,
            "entries": decoded["entries"],
        }
    )
    return base


def _is_client_route(path: str) -> bool:
    if path == "/" or path == "/campaigns":
        return True
    return path.startswith("/campaigns/")


class UIRequestHandler(SimpleHTTPRequestHandler):
    """Serves the SPA, static assets, and the read-only artifact API."""

    server_version = "CosmosUI/1"
    sys_version = ""
    protocol_version = "HTTP/1.1"
    extensions_map = {
        **SimpleHTTPRequestHandler.extensions_map,
        ".html": "text/html; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".js": "text/javascript; charset=utf-8",
        ".mjs": "text/javascript; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".svg": "image/svg+xml",
        ".webmanifest": "application/manifest+json; charset=utf-8",
    }

    error_message_format = (
        '<!DOCTYPE html><html><head><meta charset="utf-8"><title>Error response</title>'
        "</head><body><h1>Error response</h1><p>Error code: %(code)d</p>"
        "<p>Message: %(message)s.</p>"
        "<p>Error code explanation: %(code)s - %(explain)s.</p></body></html>"
    )

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = self.path.split("?", 1)[0]
        if not self._host_is_allowed():
            self._reject_host(path, head=False)
            return
        if path == "/api" or path.startswith("/api/"):
            self._handle_api(path, head=False)
            return
        self._serve_static(path, head=False)

    def do_HEAD(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = self.path.split("?", 1)[0]
        if not self._host_is_allowed():
            self._reject_host(path, head=True)
            return
        if path == "/api" or path.startswith("/api/"):
            self._handle_api(path, head=True)
            return
        self._serve_static(path, head=True)

    def _host_is_allowed(self) -> bool:
        if getattr(self.server, "allow_remote", False):
            return True
        raw = self.headers.get("Host")
        if not raw:
            return False
        try:
            hostname = (urllib.parse.urlsplit(f"//{raw}").hostname or "").rstrip(".").lower()
        except ValueError:
            return False
        if hostname == "localhost":
            return True
        try:
            address = ipaddress.ip_address(hostname)
        except ValueError:
            return False
        if address.is_loopback:
            return True
        mapped = getattr(address, "ipv4_mapped", None)
        return bool(mapped and mapped.is_loopback)

    def _reject_host(self, path: str, head: bool) -> None:
        if path == "/api" or path.startswith("/api/"):
            self._send_json(
                HTTPStatus.FORBIDDEN,
                {
                    "schema_version": SCHEMA_VERSION,
                    "error": {
                        "code": "forbidden_host",
                        "message": "this local server only accepts loopback Host headers",
                    },
                },
                head=head,
            )
            return
        self.send_error(HTTPStatus.FORBIDDEN, "Forbidden")

    def _serve_static(self, path: str, head: bool) -> None:
        decoded = urllib.parse.unquote(path)
        if any(ord(ch) < 0x20 for ch in decoded) or "\ufffd" in decoded:
            self.send_error(HTTPStatus.BAD_REQUEST, "Bad request")
            return
        client_route = _is_client_route(decoded)
        if not client_route and any(
            part.startswith(".") for part in decoded.split("/") if part
        ):
            self.send_error(HTTPStatus.NOT_FOUND, "File not found")
            return
        self.path = "/index.html" if client_route else path
        try:
            if head:
                super().do_HEAD()
            else:
                super().do_GET()
        except (ValueError, OSError):
            self.close_connection = True

    def _handle_api(self, path: str, head: bool) -> None:
        try:
            segments = [urllib.parse.unquote(part) for part in path.split("/") if part]
            if segments[:1] != ["api"]:
                raise ArtifactError(HTTPStatus.NOT_FOUND, "not_found", "unknown API route")
            if len(segments) == 2 and segments[1] == "campaigns":
                payload = _catalog(self.server.artifact_root)
            elif len(segments) >= 3 and segments[1] == "campaigns":
                campaign_id = segments[2]
                if len(segments) == 3:
                    payload = _campaign_payload(self.server.artifact_root, campaign_id)
                elif len(segments) == 4 and segments[3] == "universes":
                    payload = _universes_payload(self.server.artifact_root, campaign_id)
                elif len(segments) == 5 and segments[3] == "universes":
                    payload = _universe_payload(
                        self.server.artifact_root, campaign_id, segments[4]
                    )
                elif (
                    len(segments) == 6
                    and segments[3] == "universes"
                    and segments[5] == "trace"
                ):
                    payload = _trace_payload(
                        self.server.artifact_root, campaign_id, segments[4]
                    )
                else:
                    raise ArtifactError(
                        HTTPStatus.NOT_FOUND, "not_found", "unknown API route"
                    )
            else:
                raise ArtifactError(HTTPStatus.NOT_FOUND, "not_found", "unknown API route")
        except ArtifactError as exc:
            self._send_json(
                exc.status,
                {
                    "schema_version": SCHEMA_VERSION,
                    "error": {"code": exc.code, "message": exc.message},
                },
                head=head,
            )
            return
        except Exception as exc:  # keep a malformed artifact from killing the server
            if not getattr(self.server, "quiet", False):
                self.log_error("unhandled API error: %s", exc)
            self._send_json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                {
                    "schema_version": SCHEMA_VERSION,
                    "error": {
                        "code": "internal_error",
                        "message": "the artifact server could not complete this request",
                    },
                },
                head=head,
            )
            return
        self._send_json(HTTPStatus.OK, payload, head=head)

    def _send_json(self, status: int, payload: dict[str, Any], head: bool) -> None:
        try:
            body = json.dumps(
                _sanitize_json(payload),
                ensure_ascii=False,
                separators=(",", ":"),
                allow_nan=False,
            ).encode("utf-8")
        except (TypeError, ValueError, UnicodeError, RecursionError, MemoryError) as exc:
            if not getattr(self.server, "quiet", False):
                self.log_error("could not serialize API response: %s", exc)
            status = HTTPStatus.INTERNAL_SERVER_ERROR
            body = json.dumps(
                {
                    "schema_version": SCHEMA_VERSION,
                    "error": {
                        "code": "internal_error",
                        "message": "the artifact server could not encode this response",
                    },
                },
                separators=(",", ":"),
                ensure_ascii=True,
            ).encode("ascii")
        try:
            self.send_response(int(status))
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head:
                self.wfile.write(body)
        except OSError:
            self.close_connection = True

    def list_directory(self, path: str) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.send_error(HTTPStatus.NOT_FOUND, "File not found")
        return None

    def end_headers(self) -> None:
        for name, value in SECURITY_HEADERS.items():
            self.send_header(name, value)
        super().end_headers()

    def log_message(self, format: str, *args: Any) -> None:
        if getattr(self.server, "quiet", False):
            return
        super().log_message(format, *args)


def create_server(
    artifact_root: Path,
    web_root: Path,
    host: str = "127.0.0.1",
    port: int = 8000,
    quiet: bool = False,
    allow_remote: bool = False,
) -> ThreadingHTTPServer:
    """Create (but do not start) the UI server. Exposed for tests and ``main``."""
    artifact_root = artifact_root.resolve()
    web_root = web_root.resolve()
    if not artifact_root.is_dir():
        raise ValueError(f"artifact root is not a directory: {artifact_root}")
    if not (web_root / "index.html").is_file():
        raise ValueError(f"web root has no index.html: {web_root}")
    handler = functools.partial(UIRequestHandler, directory=str(web_root))
    server = ThreadingHTTPServer((host, port), handler)
    server.artifact_root = artifact_root  # type: ignore[attr-defined]
    server.web_root = web_root  # type: ignore[attr-defined]
    server.quiet = quiet  # type: ignore[attr-defined]
    server.allow_remote = allow_remote  # type: ignore[attr-defined]
    return server


def _display_host(host: str) -> str:
    if ":" in host and not host.startswith("["):
        return f"[{host}]"
    return host


def main(argv: list[str] | None = None) -> int:
    ui_dir = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=ui_dir / "fixtures",
        help="artifact root containing campaign directories (default: ui/fixtures)",
    )
    parser.add_argument(
        "--web-root",
        type=Path,
        default=ui_dir / "web",
        help="static web root (default: ui/web)",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="bind address; loopback by default (default: 127.0.0.1)",
    )
    parser.add_argument("--port", type=int, default=8000, help="TCP port (default: 8000)")
    parser.add_argument(
        "--allow-remote",
        action="store_true",
        help="allow a non-loopback bind; there is no authentication",
    )
    parser.add_argument("--quiet", action="store_true", help="suppress per-request logging")
    args = parser.parse_args(argv)

    if not is_loopback(args.host) and not args.allow_remote:
        parser.error(
            "refusing a non-loopback bind without --allow-remote; the v1 UI has no "
            "authentication"
        )
    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")

    try:
        server = create_server(
            args.root,
            args.web_root,
            host=args.host,
            port=args.port,
            quiet=args.quiet,
            allow_remote=args.allow_remote,
        )
    except (OSError, ValueError) as exc:
        print(f"cosmos-ui: {exc}", file=sys.stderr)
        return 1

    bound_host, bound_port = server.server_address[:2]
    print(
        f"Cosmos UI: http://{_display_host(str(bound_host))}:{bound_port}/  "
        f"(artifact root: {args.root})",
        file=sys.stderr,
    )
    if not is_loopback(str(bound_host)):
        print(
            "cosmos-ui: warning: bound to a non-loopback address with no authentication",
            file=sys.stderr,
        )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        print("\ncosmos-ui: stopped", file=sys.stderr)
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
