#!/usr/bin/env python3
"""Local, read-only artifact server for the Cosmos UI.

The simulation binary never hosts HTTP (docs/ui-design.md D4). This process reads one
campaign artifact root written by ``include/cosmos/export.hpp`` and serves:

* ``ui/web`` as the static application,
* a small JSON API over the schema-v1 artifact layout,
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
    if value > MAX_SEED:
        raise ArtifactError(HTTPStatus.BAD_REQUEST, "invalid_seed", "seed is wider than 64 bits")
    return value


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
    seed = parse_seed(seed_text)
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
    rows = _read_universe_rows_required(campaign_dir)
    row = next((item for item in rows if int(item["seed"]) == seed), None)
    if row is None:
        raise ArtifactError(
            HTTPStatus.NOT_FOUND,
            "universe_not_found",
            f"universe with seed {seed_text!r} not found",
        )
    detail_path = _artifact_file(campaign_dir, "universes", f"{row['index']}.json")
    detail = _read_json_object(detail_path, f"universes/{row['index']}.json")
    if detail.get("index") != row["index"] or detail.get("seed") != row["seed"]:
        raise ArtifactError(
            HTTPStatus.INTERNAL_SERVER_ERROR,
            "universe_identity_mismatch",
            f"universes/{row['index']}.json does not match its index row",
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "campaign_id": campaign_id,
        "manifest": manifest,
        "campaign": campaign,
        "universe": detail,
    }


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
