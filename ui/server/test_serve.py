#!/usr/bin/env python3
"""Contract tests for the local artifact server (stdlib only).

Run with ``just ui-test`` or directly:
    python3 -m unittest discover -s ui/server -p 'test_*.py' -v
"""

from __future__ import annotations

import http.client
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serve  # noqa: E402 - test bootstrap above

UI_DIR = Path(__file__).resolve().parent.parent
FIXTURE_ROOT = UI_DIR / "fixtures"
WEB_ROOT = UI_DIR / "web"
DEMO_SEED = "16860738450190168606"
DEMO_SEED_HEX = "0xe9fd6049d65af21e"


class ServerCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = serve.create_server(FIXTURE_ROOT, WEB_ROOT, "127.0.0.1", 0, quiet=True)
        cls.thread = threading.Thread(
            target=cls.server.serve_forever,
            kwargs={"poll_interval": 0.02},
            daemon=True,
        )
        cls.thread.start()
        cls.host, cls.port = cls.server.server_address[:2]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def request(self, path: str, method: str = "GET"):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            body = response.read()
            return response.status, {key.lower(): value for key, value in response.getheaders()}, body
        finally:
            connection.close()

    def request_with_host(self, path: str, host: str):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", path, headers={"Host": host})
            response = connection.getresponse()
            body = response.read()
            return response.status, {}, json.loads(body.decode("utf-8"))
        finally:
            connection.close()

    def json_request(self, path: str, method: str = "GET"):
        status, headers, body = self.request(path, method)
        return status, headers, json.loads(body.decode("utf-8"))

    def test_catalog_lists_demo(self) -> None:
        status, _, payload = self.json_request("/api/campaigns")
        self.assertEqual(status, 200)
        self.assertEqual(payload["schema_version"], 1)
        campaigns = payload["campaigns"]
        self.assertEqual([entry["campaign_id"] for entry in campaigns], ["demo"])
        demo = campaigns[0]
        self.assertTrue(demo["readable"])
        self.assertEqual(demo["name"], "Cosmos demo campaign")
        self.assertEqual(demo["build"]["id"], "build-demo")
        self.assertEqual(demo["config"]["trials"], 4)
        self.assertTrue(demo["complete"])

    def test_campaign_payload_has_rows_and_findings(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/demo")
        self.assertEqual(status, 200)
        self.assertEqual(payload["manifest"]["campaign_id"], "demo")
        self.assertEqual(payload["campaign"]["campaign_id"], "demo")
        self.assertEqual(len(payload["universes"]), 4)
        self.assertTrue(all(isinstance(row["seed"], str) for row in payload["universes"]))
        self.assertEqual(
            [row["index"] for row in payload["universes"]],
            [0, 1, 2, 3],
        )
        finding = payload["campaign"]["findings"][0]
        self.assertEqual(finding["id"], "odd-index-fails")
        self.assertIn(DEMO_SEED, finding["seeds"])

    def test_universes_endpoint(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/demo/universes")
        self.assertEqual(status, 200)
        self.assertEqual(len(payload["universes"]), 4)
        self.assertEqual(payload["manifest"]["campaign_id"], "demo")

    def test_universe_detail_by_decimal_seed(self) -> None:
        status, _, payload = self.json_request(f"/api/campaigns/demo/universes/{DEMO_SEED}")
        self.assertEqual(status, 200)
        universe = payload["universe"]
        self.assertEqual(universe["index"], 1)
        self.assertEqual(universe["seed"], DEMO_SEED)
        self.assertEqual(universe["status"], "failed")
        self.assertEqual(universe["checks"][0]["passed"], False)
        self.assertEqual(universe["ledger"][0]["site"], "malloc")
        self.assertEqual(payload["campaign"]["campaign_id"], "demo")
        self.assertEqual(payload["campaign"]["findings"][0]["id"], "odd-index-fails")

    def test_universe_detail_by_hex_seed(self) -> None:
        status, _, payload = self.json_request(f"/api/campaigns/demo/universes/{DEMO_SEED_HEX}")
        self.assertEqual(status, 200)
        self.assertEqual(payload["universe"]["seed"], DEMO_SEED)

    def test_unknown_campaign_is_404_json(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/not-there")
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"]["code"], "campaign_not_found")

    def test_unknown_universe_is_404_json(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/demo/universes/1")
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"]["code"], "universe_not_found")

    def test_invalid_seed_is_400(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/demo/universes/not-a-seed")
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"]["code"], "invalid_seed")

    def test_campaign_id_traversal_is_rejected(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/..%2F..%2Fetc")
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"]["code"], "invalid_campaign_id")

    def test_seed_traversal_is_rejected(self) -> None:
        status, _, payload = self.json_request(
            "/api/campaigns/demo/universes/..%2F..%2Ftraces%2F0.bin"
        )
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"]["code"], "invalid_seed")

    def test_unknown_api_route_is_404_json(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/demo/unknown")
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"]["code"], "not_found")

    def test_spa_fallback_and_security_headers(self) -> None:
        status, headers, body = self.request("/campaigns/demo")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers["content-type"])
        self.assertIn(b'id="main"', body)
        self.assertIn("default-src 'self'", headers["content-security-policy"])
        self.assertEqual(headers["cache-control"], "no-store")
        self.assertEqual(headers["x-content-type-options"], "nosniff")

    def test_static_module_has_javascript_content_type(self) -> None:
        status, headers, body = self.request("/app.js")
        self.assertEqual(status, 200)
        self.assertIn("text/javascript", headers["content-type"])
        self.assertIn(b"installLinkHandler", body)

    def test_missing_asset_is_404_not_spa(self) -> None:
        status, headers, body = self.request("/nope.js")
        self.assertEqual(status, 404)
        self.assertNotIn(b'id="main"', body)

    def test_directory_listing_is_disabled(self) -> None:
        status, _, body = self.request("/lib/")
        self.assertEqual(status, 404)
        self.assertNotIn(b"index of", body.lower())

    def test_dot_prefixed_campaign_route_uses_spa_fallback(self) -> None:
        status, headers, body = self.request("/campaigns/.dotdemo")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers["content-type"])
        self.assertIn(b'id="main"', body)

    def test_dotfile_outside_client_routes_is_blocked(self) -> None:
        status, _, _ = self.request("/.git/config")
        self.assertEqual(status, 404)

    def test_head_api_has_length_and_no_body(self) -> None:
        status, headers, body = self.request("/api/campaigns", method="HEAD")
        self.assertEqual(status, 200)
        self.assertGreater(int(headers["content-length"]), 0)
        self.assertEqual(body, b"")

    def test_non_get_method_is_not_implemented(self) -> None:
        status, _, _ = self.request("/api/campaigns", method="POST")
        self.assertEqual(status, 501)

    def test_non_loopback_host_is_rejected(self) -> None:
        status, _, payload = self.request_with_host(
            "/api/campaigns", "attacker.example:8000"
        )
        self.assertEqual(status, 403)
        self.assertEqual(payload["error"]["code"], "forbidden_host")

    def test_ipv4_mapped_loopback_host_is_allowed(self) -> None:
        status, _, payload = self.request_with_host(
            "/api/campaigns", "[::ffff:127.0.0.1]:8000"
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["schema_version"], 1)

    def test_non_loopback_host_is_rejected_for_static(self) -> None:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", "/", headers={"Host": "attacker.example:8000"})
            response = connection.getresponse()
            self.assertEqual(response.status, 403)
            response.read()
        finally:
            connection.close()

    def test_nul_static_path_is_rejected(self) -> None:
        status, _, _ = self.request("/index.html%00.js")
        self.assertEqual(status, 400)

    def test_surrogate_encoded_static_path_is_rejected(self) -> None:
        for method in ("GET", "HEAD"):
            status, _, _ = self.request("/%ed%a0%80", method=method)
            self.assertEqual(status, 400)


class BrokenArtifactCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        broken = cls.root / "broken"
        broken.mkdir()
        (broken / "manifest.json").write_text(
            json.dumps({"schema_version": 99, "campaign_id": "broken"}),
            encoding="utf-8",
        )
        partial = cls.root / "partial"
        partial.mkdir()
        (partial / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "campaign_id": "partial",
                    "name": "Partial campaign",
                    "complete": False,
                    "error": "worker 3 died before flush",
                    "build": {"id": "partial-build", "mode": "SIM"},
                    "config": {"trials": 1},
                }
            ),
            encoding="utf-8",
        )
        cls.server = serve.create_server(cls.root, WEB_ROOT, "127.0.0.1", 0, quiet=True)
        cls.thread = threading.Thread(
            target=cls.server.serve_forever,
            kwargs={"poll_interval": 0.02},
            daemon=True,
        )
        cls.thread.start()
        cls.host, cls.port = cls.server.server_address[:2]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temp.cleanup()

    def request_json(self, path: str):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            body = response.read()
            return response.status, json.loads(body.decode("utf-8"))
        finally:
            connection.close()

    def test_broken_campaign_appears_as_catalog_error(self) -> None:
        status, payload = self.request_json("/api/campaigns")
        self.assertEqual(status, 200)
        entries = {entry["campaign_id"]: entry for entry in payload["campaigns"]}
        self.assertEqual(set(entries), {"broken", "partial"})
        broken = entries["broken"]
        self.assertFalse(broken["readable"])
        self.assertIn("catalog_error", broken)
        self.assertNotIn("build", broken)
        self.assertNotIn("error", broken)

    def test_readable_incomplete_campaign_is_not_catalog_error(self) -> None:
        status, payload = self.request_json("/api/campaigns")
        self.assertEqual(status, 200)
        partial = {entry["campaign_id"]: entry for entry in payload["campaigns"]}["partial"]
        self.assertTrue(partial["readable"])
        self.assertFalse(partial["complete"])
        self.assertEqual(partial["error"], "worker 3 died before flush")
        self.assertEqual(partial["build"]["id"], "partial-build")

    def test_broken_campaign_is_500_json(self) -> None:
        status, payload = self.request_json("/api/campaigns/broken")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "unsupported_schema")

    def test_manifest_only_campaign_is_incomplete_not_404(self) -> None:
        status, payload = self.request_json("/api/campaigns/partial")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_incomplete")

    def test_manifest_only_universes_endpoint_is_incomplete_not_404(self) -> None:
        status, payload = self.request_json("/api/campaigns/partial/universes")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_incomplete")


class NonFiniteArtifactCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        (cls.root / "nan").mkdir()
        (cls.root / "nan" / "manifest.json").write_text(
            json.dumps({"schema_version": 1, "campaign_id": "nan"}),
            encoding="utf-8",
        )
        (cls.root / "nan" / "campaign.json").write_text(
            '{"schema_version": 1, "campaign_id": "nan", "runs": NaN}',
            encoding="utf-8",
        )
        (cls.root / "nanrows").mkdir()
        (cls.root / "nanrows" / "manifest.json").write_text(
            json.dumps({"schema_version": 1, "campaign_id": "nanrows"}),
            encoding="utf-8",
        )
        (cls.root / "nanrows" / "campaign.json").write_text(
            json.dumps({"schema_version": 1, "campaign_id": "nanrows"}),
            encoding="utf-8",
        )
        (cls.root / "nanrows" / "universes.ndjson").write_text(
            '{"schema_version": 1, "index": 0, "seed": "1", "wall_ns": NaN}\n',
            encoding="utf-8",
        )
        depth = 1200
        (cls.root / "deep").mkdir()
        (cls.root / "deep" / "manifest.json").write_text(
            json.dumps({"schema_version": 1, "campaign_id": "deep"}),
            encoding="utf-8",
        )
        (cls.root / "deep" / "campaign.json").write_text(
            '{"schema_version": 1, "campaign_id": "deep", "x": '
            + ('{"x": ' * depth)
            + "0"
            + ("}" * depth)
            + "}",
            encoding="utf-8",
        )
        (cls.root / "deep" / "universes.ndjson").write_text("", encoding="utf-8")
        (cls.root / "surrogate").mkdir()
        (cls.root / "surrogate" / "manifest.json").write_text(
            '{"schema_version": 1, "campaign_id": "surrogate", "name": "\\ud800"}',
            encoding="utf-8",
        )
        cls.server = serve.create_server(cls.root, WEB_ROOT, "127.0.0.1", 0, quiet=True)
        cls.thread = threading.Thread(
            target=cls.server.serve_forever,
            kwargs={"poll_interval": 0.02},
            daemon=True,
        )
        cls.thread.start()
        cls.host, cls.port = cls.server.server_address[:2]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temp.cleanup()

    def request_json(self, path: str):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            body = response.read()
            return response.status, json.loads(body.decode("utf-8"))
        finally:
            connection.close()

    def test_non_finite_campaign_is_a_structured_500(self) -> None:
        status, payload = self.request_json("/api/campaigns/nan")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_malformed")

    def test_non_finite_rows_are_a_structured_500(self) -> None:
        status, payload = self.request_json("/api/campaigns/nanrows/universes")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_malformed")

    def test_lone_surrogate_is_replaced_in_the_response(self) -> None:
        status, payload = self.request_json("/api/campaigns")
        self.assertEqual(status, 200)
        entry = next(
            item for item in payload["campaigns"] if item["campaign_id"] == "surrogate"
        )
        self.assertEqual(entry["name"], "\ufffd")

    def test_deeply_nested_campaign_is_a_structured_500(self) -> None:
        status, payload = self.request_json("/api/campaigns/deep")
        self.assertEqual(status, 500)
        self.assertIn(
            payload["error"]["code"],
            {"artifact_too_deep", "internal_error", "artifact_malformed"},
        )

    def test_server_remains_responsive_after_deep_artifact(self) -> None:
        status, payload = self.request_json("/api/campaigns")
        self.assertEqual(status, 200)
        self.assertEqual(payload["schema_version"], 1)


class DotfileCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.web = Path(cls.temp.name) / "web"
        cls.web.mkdir()
        (cls.web / "index.html").write_text("<!doctype html><main id=\"main\"></main>", encoding="utf-8")
        (cls.web / ".env").write_text("SECRET_TOKEN=abc123", encoding="utf-8")
        (cls.web / ".git").mkdir()
        (cls.web / ".git" / "config").write_text("[core] secret", encoding="utf-8")
        cls.server = serve.create_server(FIXTURE_ROOT, cls.web, "127.0.0.1", 0, quiet=True)
        cls.thread = threading.Thread(
            target=cls.server.serve_forever,
            kwargs={"poll_interval": 0.02},
            daemon=True,
        )
        cls.thread.start()
        cls.host, cls.port = cls.server.server_address[:2]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temp.cleanup()

    def request(self, path: str):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def test_literal_dotfile_is_blocked(self) -> None:
        status, body = self.request("/.env")
        self.assertEqual(status, 404)
        self.assertNotIn(b"SECRET_TOKEN", body)

    def test_percent_encoded_dotfile_is_blocked(self) -> None:
        for path in ("/%2eenv", "/%2e%65nv"):
            status, body = self.request(path)
            self.assertEqual(status, 404)
            self.assertNotIn(b"SECRET_TOKEN", body)

    def test_percent_encoded_dot_directory_is_blocked(self) -> None:
        status, body = self.request("/%2egit%2fconfig")
        self.assertEqual(status, 404)
        self.assertNotIn(b"secret", body)

    def test_encoded_parent_traversal_to_dotfile_is_blocked(self) -> None:
        status, body = self.request("/x%2f..%2f.env")
        self.assertEqual(status, 404)
        self.assertNotIn(b"SECRET_TOKEN", body)

    def test_dot_campaign_route_still_uses_spa(self) -> None:
        status, body = self.request("/campaigns/.dotdemo")
        self.assertEqual(status, 200)
        self.assertIn(b'id="main"', body)


if __name__ == "__main__":
    unittest.main()
