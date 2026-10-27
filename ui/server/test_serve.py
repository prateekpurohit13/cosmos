#!/usr/bin/env python3
"""Contract tests for the local artifact server (stdlib only).

Run with ``just ui-test`` or directly:
    python3 -m unittest discover -s ui/server -p 'test_*.py' -v
"""

from __future__ import annotations

import http.client
import json
import struct
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
DEMO_PASS_SEED = "627405149472732430"

TRACE_HEADER = struct.Struct("<BQQ")
TRACE_ENTRY = struct.Struct("<QQqQqBBB")
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def encode_trace(entries, seed=42, source=1):
    data = TRACE_HEADER.pack(source, len(entries), seed)
    for entry in entries:
        data += TRACE_ENTRY.pack(*entry)
    return data


def trace_hash(data):
    digest = FNV_OFFSET
    for byte in data:
        digest ^= byte
        digest = (digest * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return f"0x{digest:016x}"


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

    def test_fixture_trace_decodes_canonical_bytes(self) -> None:
        status, _, payload = self.json_request(
            f"/api/campaigns/demo/universes/{DEMO_SEED}/trace"
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["campaign_id"], "demo")
        self.assertEqual(payload["universe"]["index"], 1)
        trace = payload["trace"]
        self.assertTrue(trace["present"])
        self.assertFalse(trace["missing"])
        self.assertEqual(trace["path"], "traces/1.bin")
        self.assertEqual(trace["count"], 24)
        self.assertEqual(trace["size_bytes"], 17 + 24 * 43)
        self.assertEqual(trace["declared_hash"], "0xb8b48f277dcd5d9d")
        self.assertEqual(trace["declared_hash"], trace["computed_hash"])
        self.assertTrue(trace["hash_matches"])
        self.assertTrue(trace["seed_matches"])
        self.assertEqual(len(trace["entries"]), 24)
        entry = trace["entries"][0]
        self.assertEqual(entry["seq"], 1)
        self.assertEqual(entry["site"], "malloc")
        self.assertEqual(entry["site_id"], 1)
        self.assertEqual(entry["fault_class"], "Memory")
        self.assertEqual(entry["outcome"], "OutOfMemory")
        self.assertEqual(entry["status"], "FIRED")
        self.assertTrue(entry["drew"])
        self.assertEqual(entry["eligible_index"], 1)
        injected = [item for item in trace["entries"] if item["outcome"] != "None"]
        self.assertTrue(injected)
        self.assertEqual(injected[0]["outcome"], "OutOfMemory")
        self.assertTrue(any(item["fault_class"] == "Clock" for item in injected))
        self.assertTrue(any(item["outcome"] == "None" for item in trace["entries"]))

    def test_fixture_minimization_report_is_1_minimal(self) -> None:
        status, _, payload = self.json_request(
            f"/api/campaigns/demo/universes/{DEMO_SEED}/minimization"
        )
        self.assertEqual(status, 200)
        self.assertEqual(payload["universe"]["seed"], DEMO_SEED)
        report = payload["minimization"]
        self.assertTrue(report["present"])
        self.assertEqual(report["path"], f"minimizations/{DEMO_SEED}.json")
        self.assertEqual(report["finding_id"], "odd-index-fails")
        self.assertFalse(report["refused"])
        self.assertEqual(len(report["candidates"]), 11)
        self.assertEqual(len(report["kept"]), 1)
        kept = report["kept"][0]
        self.assertEqual(report["only_faults"], [kept["seq"]])
        self.assertEqual(kept["site"], "malloc")
        self.assertEqual(kept["outcome"], "OutOfMemory")
        self.assertEqual(kept["status"], "FIRED")
        self.assertIn(f"--only-faults={kept['seq']}", report["repro"])
        self.assertEqual(report["passes"], 2)
        self.assertEqual(report["replays"], 13)
        self.assertEqual(report["crashed_trials"], 0)

    def test_missing_minimization_report_is_explicit_not_empty_success(self) -> None:
        status, _, payload = self.json_request(
            f"/api/campaigns/demo/universes/{DEMO_PASS_SEED}/minimization"
        )
        self.assertEqual(status, 200)
        report = payload["minimization"]
        self.assertFalse(report["present"])
        self.assertEqual(report["path"], f"minimizations/{DEMO_PASS_SEED}.json")
        self.assertIsNone(report["finding_id"])
        self.assertIsNone(report["repro"])
        self.assertEqual(report["candidates"], [])
        self.assertEqual(report["kept"], [])

    def test_minimization_endpoint_unknown_universe_is_404(self) -> None:
        status, _, payload = self.json_request(
            "/api/campaigns/demo/universes/123/minimization"
        )
        self.assertEqual(status, 404)
        self.assertEqual(payload["error"]["code"], "universe_not_found")

    def test_minimization_endpoint_rejects_invalid_seed(self) -> None:
        status, _, payload = self.json_request(
            "/api/campaigns/demo/universes/nope/minimization"
        )
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"]["code"], "invalid_seed")

    def test_trace_endpoint_rejects_an_invalid_seed(self) -> None:
        status, _, payload = self.json_request("/api/campaigns/demo/universes/nope/trace")
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"]["code"], "invalid_seed")

    def test_huge_seed_is_a_400_not_a_500(self) -> None:
        status, _, payload = self.json_request(
            "/api/campaigns/demo/universes/" + "9" * 5000 + "/trace"
        )
        self.assertEqual(status, 400)
        self.assertEqual(payload["error"]["code"], "invalid_seed")

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

    def test_compare_route_uses_spa_fallback(self) -> None:
        status, headers, body = self.request(
            f"/campaigns/demo/compare?left={DEMO_SEED}&right={DEMO_SEED_HEX}"
        )
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers["content-type"])
        self.assertIn(b'id="main"', body)
        self.assertIn("default-src 'self'", headers["content-security-policy"])

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


    def test_fixture_events_decode(self) -> None:
        status, _, payload = self.json_request(
            f"/api/campaigns/demo/universes/{DEMO_SEED}/events"
        )
        self.assertEqual(status, 200)
        events = payload["events"]
        self.assertTrue(events["present"])
        self.assertEqual(events["path"], "events/1.bin")
        self.assertEqual(events["source"], 3)
        self.assertEqual(events["version"], 1)
        self.assertGreater(events["count"], 10)
        self.assertEqual(events["dropped"], 0)
        self.assertIsNone(events["gap"])
        kinds = {entry["kind"] for entry in events["entries"]}
        self.assertIn("scheduler.spawn", kinds)
        self.assertIn("network.send", kinds)
        self.assertIn("memory.alloc", kinds)
        self.assertIn("clock.gettime", kinds)
        self.assertIn("application.counter", kinds)
        self.assertEqual(
            [entry["seq"] for entry in events["entries"]],
            list(range(events["first_seq"], events["next_seq"])),
        )

class TraceArtifactCase(unittest.TestCase):
    """A synthetic artifact for trace variants the demo fixture does not exercise."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        campaign = cls.root / "trace"
        (campaign / "universes").mkdir(parents=True)
        (campaign / "traces").mkdir()
        (campaign / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "campaign_id": "trace",
                    "name": "Trace variants",
                    "complete": True,
                    "build": {"id": "trace-build", "mode": "SIM"},
                    "config": {"trials": 7, "trace": True, "trace_capacity": 4096},
                }
            ),
            encoding="utf-8",
        )
        (campaign / "campaign.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "campaign_id": "trace",
                    "name": "Trace variants",
                    "runs": 7,
                }
            ),
            encoding="utf-8",
        )

        entries = [
            (1, 1, 0, 1, 0, 1, 1, 0),
            (2, 2, 1_000, 12, 500, 16, 1, 0),
            (3, 3, 2_000, 6, 0, 0, 0, 0),
            (4, 4, 3_000, 10, 0, 12, 1, 1),
            (5, 5, 4_000, 1000, 0, 0, 0, 0),
        ]
        good = encode_trace(entries, seed=42)
        wrong_hash = encode_trace(entries, seed=45)
        seed_mismatch = encode_trace(entries, seed=99)
        bad_source = encode_trace(entries, seed=46, source=2)
        truncated = good[:-1]
        trailing = good + b"\x00"
        path_mismatch = encode_trace(entries, seed=49)
        too_large = TRACE_HEADER.pack(1, serve.MAX_TRACE_ENTRIES + 1, 51)
        bad_site = encode_trace([(1, 1, 0, 9999, 0, 1, 1, 0)], seed=52)
        bad_outcome = encode_trace([(1, 1, 0, 1, 0, 20, 1, 0)], seed=53)
        bad_drew = encode_trace([(1, 1, 0, 1, 0, 1, 2, 0)], seed=54)
        bad_status = encode_trace([(1, 1, 0, 1, 0, 1, 1, 3)], seed=55)
        padded_seed = encode_trace(entries, seed=99)
        empty_trace = TRACE_HEADER.pack(1, 0, 57)
        short_header = b"\x01\x00"
        unsafe_integer = encode_trace([(2**53, 1, 0, 1, 0, 1, 1, 0)], seed=59)
        missing_path = encode_trace([(1, 1, 0, 1, 0, 1, 1, 0)], seed=60)
        duplicate_seq = encode_trace(
            [(1, 1, 0, 1, 0, 1, 1, 0), (1, 2, 1_000, 1, 0, 0, 1, 0)], seed=61
        )

        variants = {
            0: ("42", True, "traces/0.bin", good, trace_hash(good)),
            1: ("43", True, "traces/1.bin", None, "0x0"),
            2: ("44", False, "", b"", None),
            3: ("45", True, "traces/3.bin", wrong_hash, "0xdeadbeefdeadbeef"),
            4: ("46", True, "traces/4.bin", bad_source, trace_hash(bad_source)),
            5: ("47", True, "traces/5.bin", truncated, trace_hash(truncated)),
            6: ("48", True, "traces/6.bin", trailing, trace_hash(trailing)),
            7: ("49", True, "traces/999.bin", path_mismatch, trace_hash(path_mismatch)),
            8: ("50", True, "traces/8.bin", seed_mismatch, trace_hash(seed_mismatch)),
            9: ("51", True, "traces/9.bin", too_large, trace_hash(too_large)),
            10: ("52", True, "traces/10.bin", bad_site, trace_hash(bad_site)),
            11: ("53", True, "traces/11.bin", bad_outcome, trace_hash(bad_outcome)),
            12: ("54", True, "traces/12.bin", bad_drew, trace_hash(bad_drew)),
            13: ("55", True, "traces/13.bin", bad_status, trace_hash(bad_status)),
            14: ("00099", True, "traces/14.bin", padded_seed, trace_hash(padded_seed)),
            15: ("56", True, "traces/15.bin", None, "0x0"),
            16: ("57", True, "traces/16.bin", empty_trace, trace_hash(empty_trace)),
            17: ("58", True, "traces/17.bin", short_header, trace_hash(short_header)),
            18: ("59", True, "traces/18.bin", unsafe_integer, trace_hash(unsafe_integer)),
            19: ("60", True, "", missing_path, trace_hash(missing_path)),
            20: ("61", True, "traces/20.bin", duplicate_seq, trace_hash(duplicate_seq)),
            21: ("62", False, "", None, None),
        }
        rows = []
        for index, (seed, present, path, data, digest) in variants.items():
            trace_meta = {
                "present": present,
                "traced": present,
                "path": path,
                "hash": digest,
                "capacity": 4096,
            }
            if index != 21:
                trace_meta["dropped"] = 3 if index == 0 else 0
            rows.append(
                {
                    "schema_version": 1,
                    "index": index,
                    "seed": seed,
                    "status": "passed",
                    "trace": trace_meta,
                }
            )
            if data is not None:
                (campaign / "traces" / f"{index}.bin").write_bytes(data)
            detail = {
                "schema_version": 1,
                "index": index,
                "seed": seed,
                "seed_hex": f"0x{int(seed):016x}",
                "status": "passed",
                "virtual_end_ns": 5_000,
                "checks": [],
                "trace": trace_meta,
            }
            (campaign / "universes" / f"{index}.json").write_text(
                json.dumps(detail), encoding="utf-8"
            )
        with (campaign / "universes.ndjson").open("w", encoding="utf-8") as handle:
            for row in rows:
                handle.write(json.dumps(row) + "\n")
        # Sparse file: the size check must refuse it before read_bytes() materializes it.
        with (campaign / "traces" / "15.bin").open("wb") as handle:
            handle.truncate(serve.MAX_TRACE_BYTES + 1)

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

    def trace(self, seed: str):
        return self.request_json(f"/api/campaigns/trace/universes/{seed}/trace")

    def test_decodes_every_entry_in_order(self) -> None:
        status, payload = self.trace("42")
        self.assertEqual(status, 200)
        trace = payload["trace"]
        self.assertTrue(trace["present"])
        self.assertTrue(trace["hash_matches"])
        self.assertTrue(trace["seed_matches"])
        self.assertEqual(trace["count"], 5)
        self.assertEqual([entry["seq"] for entry in trace["entries"]], [1, 2, 3, 4, 5])
        self.assertEqual(
            [entry["fault_class"] for entry in trace["entries"]],
            ["Memory", "Clock", "Storage", "Network", "Process"],
        )
        self.assertEqual(trace["entries"][1]["amount_ns"], 500)
        self.assertEqual(trace["entries"][2]["outcome"], "None")
        self.assertEqual(trace["entries"][3]["status"], "SUPPRESSED")
        self.assertEqual(trace["entries"][4]["site"], "crash_node")
        self.assertEqual(trace["dropped"], 3)

    def test_hash_mismatch_is_reported_without_hiding_entries(self) -> None:
        status, payload = self.trace("45")
        self.assertEqual(status, 200)
        trace = payload["trace"]
        self.assertTrue(trace["present"])
        self.assertFalse(trace["hash_matches"])
        self.assertEqual(trace["declared_hash"], "0xdeadbeefdeadbeef")
        self.assertEqual(trace["count"], 5)
        self.assertEqual(len(trace["entries"]), 5)

    def test_seed_mismatch_is_reported(self) -> None:
        status, payload = self.trace("50")
        self.assertEqual(status, 200)
        self.assertFalse(payload["trace"]["seed_matches"])
        self.assertEqual(payload["trace"]["seed"], "99")

    def test_missing_file_is_explicit(self) -> None:
        status, payload = self.trace("43")
        self.assertEqual(status, 200)
        trace = payload["trace"]
        self.assertFalse(trace["present"])
        self.assertTrue(trace["missing"])
        self.assertEqual(trace["error"], "trace_missing")
        self.assertEqual(trace["entries"], [])

    def test_untraced_universe_has_no_entries(self) -> None:
        status, payload = self.trace("44")
        self.assertEqual(status, 200)
        trace = payload["trace"]
        self.assertFalse(trace["present"])
        self.assertFalse(trace["missing"])
        self.assertFalse(trace["traced"])
        self.assertEqual(trace["entries"], [])

    def test_bad_source_is_a_structured_error(self) -> None:
        status, payload = self.trace("46")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_trace_malformed")
        self.assertIn("source", payload["error"]["message"])

    def test_truncated_and_trailing_bytes_are_structured_errors(self) -> None:
        for seed, fragment in (("47", "truncated"), ("48", "trailing")):
            status, payload = self.trace(seed)
            self.assertEqual(status, 500)
            self.assertEqual(payload["error"]["code"], "artifact_trace_malformed")
            self.assertIn(fragment, payload["error"]["message"])

    def test_trace_path_mismatch_is_rejected(self) -> None:
        status, payload = self.trace("49")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "trace_path_mismatch")

    def test_too_large_trace_is_refused(self) -> None:
        status, payload = self.trace("51")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_trace_too_large")
        self.assertIn(str(serve.MAX_TRACE_ENTRIES), payload["error"]["message"])

    def test_bad_entry_values_are_structured_errors(self) -> None:
        for seed, fragment in (
            ("52", "unknown site"),
            ("53", "unknown outcome"),
            ("54", "invalid drew"),
            ("55", "unknown status"),
        ):
            status, payload = self.trace(seed)
            self.assertEqual(status, 500)
            self.assertEqual(payload["error"]["code"], "artifact_trace_malformed")
            self.assertIn(fragment, payload["error"]["message"])

    def test_equivalent_decimal_seed_does_not_false_alarm(self) -> None:
        status, payload = self.trace("00099")
        self.assertEqual(status, 200)
        self.assertTrue(payload["trace"]["seed_matches"])
        self.assertEqual(payload["trace"]["seed"], "99")
        self.assertEqual(payload["universe"]["seed"], "00099")

    def test_oversized_file_is_refused_before_read(self) -> None:
        status, payload = self.trace("56")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_trace_too_large")
        self.assertIn("exceeds", payload["error"]["message"])

    def test_empty_trace_has_no_entries(self) -> None:
        status, payload = self.trace("57")
        self.assertEqual(status, 200)
        trace = payload["trace"]
        self.assertTrue(trace["present"])
        self.assertEqual(trace["count"], 0)
        self.assertEqual(trace["entries"], [])
        self.assertTrue(trace["hash_matches"])

    def test_short_header_is_a_structured_error(self) -> None:
        status, payload = self.trace("58")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_trace_malformed")
        self.assertIn("header is truncated", payload["error"]["message"])

    def test_unsafe_integer_is_a_structured_error(self) -> None:
        status, payload = self.trace("59")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_trace_malformed")
        self.assertIn("safe-integer", payload["error"]["message"])

    def test_missing_trace_path_is_rejected(self) -> None:
        status, payload = self.trace("60")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "trace_path_mismatch")

    def test_duplicate_seq_is_a_structured_error(self) -> None:
        status, payload = self.trace("61")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_trace_malformed")
        self.assertIn("non-increasing seq", payload["error"]["message"])

    def test_missing_drop_count_is_null(self) -> None:
        status, payload = self.trace("62")
        self.assertEqual(status, 200)
        self.assertFalse(payload["trace"]["present"])
        self.assertIsNone(payload["trace"]["dropped"])


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


class MinimizationArtifactCase(unittest.TestCase):
    """A synthetic artifact for MinimizationReport variants the demo fixture does not cover."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        campaign = cls.root / "minimization"
        (campaign / "universes").mkdir(parents=True)
        (campaign / "minimization_reports").mkdir()
        (campaign / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "campaign_id": "minimization",
                    "name": "Minimization variants",
                    "complete": True,
                    "build": {"id": "minimization-build", "mode": "SIM"},
                    "config": {"trials": 18, "trace": True, "trace_capacity": 4096},
                    "paths": {"minimization_dir": "minimization_reports"},
                }
            ),
            encoding="utf-8",
        )
        (campaign / "campaign.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "campaign_id": "minimization",
                    "name": "Minimization variants",
                    "runs": 18,
                }
            ),
            encoding="utf-8",
        )

        entries = [
            {
                "seq": 1,
                "eligible_index": 1,
                "at_ns": 0,
                "site": "malloc",
                "fault_class": "Memory",
                "outcome": "OutOfMemory",
                "amount_ns": 0,
                "drew": True,
                "status": "FIRED",
            },
            {
                "seq": 2,
                "eligible_index": 2,
                "at_ns": 1_000,
                "site": "clock_gettime",
                "fault_class": "Clock",
                "outcome": "ClockStep",
                "amount_ns": 50_000,
                "drew": True,
                "status": "FIRED",
            },
            {
                "seq": 5,
                "eligible_index": 3,
                "at_ns": 2_000,
                "site": "malloc",
                "fault_class": "Memory",
                "outcome": "OutOfMemory",
                "amount_ns": 0,
                "drew": True,
                "status": "FIRED",
            },
        ]

        rows = []
        for index in range(18):
            seed = 100 + index
            row = {
                "schema_version": 1,
                "index": index,
                "seed": str(seed),
                "status": "failed",
                "trace": {"present": False, "traced": False, "path": None, "hash": None},
            }
            rows.append(row)
            detail = {
                "schema_version": 1,
                "index": index,
                "seed": str(seed),
                "seed_hex": f"0x{seed:016x}",
                "status": "failed",
                "virtual_end_ns": 5_000,
                "checks": [],
                "trace": row["trace"],
            }
            (campaign / "universes" / f"{index}.json").write_text(
                json.dumps(detail), encoding="utf-8"
            )
        with (campaign / "universes.ndjson").open("w", encoding="utf-8") as handle:
            for row in rows:
                handle.write(json.dumps(row) + "\n")

        def document(seed: int, **overrides):
            value = {
                "schema_version": 1,
                "seed": str(seed),
                "seed_hex": f"0x{seed:016x}",
                "finding_id": "double-oom",
                "program": "test_minimization",
                "refused": False,
                "reason": "",
                "candidates": entries,
                "kept": [entries[1]],
                "only_faults": [2],
                "repro": f"test_minimization --seed {seed} --only-faults=2",
                "passes": 2,
                "replays": 6,
                "crashed_trials": 0,
            }
            value.update(overrides)
            return value

        docs = {
            100: document(100),
            102: document(
                102,
                refused=True,
                reason="the trace is truncated (2 entries dropped)",
                candidates=[entries[0]],
                kept=[],
                only_faults=[],
                repro=None,
                passes=0,
                replays=0,
            ),
            103: document(999),
            104: document(104, only_faults=[1]),
            105: document(105, candidates=[entries[0], entries[1]], kept=[entries[2]], only_faults=[5]),
            106: document(
                106,
                candidates=[{**entries[0], "status": "DONE"}],
                kept=[],
                only_faults=[],
                repro="test_minimization --seed 106 --only-faults=",
            ),
            107: {**document(107), "schema_version": 2},
            109: document(109, passes=-1),
            110: {**document(110), "seed": "9" * 100, "seed_hex": "0x" + "f" * 100},
            113: document(113, crashed_trials=2, replays=1),
            114: document(
                114,
                candidates=[entries[2], entries[0]],
                kept=[],
                only_faults=[],
                repro="test_minimization --seed 114 --only-faults=",
            ),
            115: document(115, finding_id=""),
            111: document(
                111,
                candidates=[],
                kept=[],
                only_faults=[],
                repro="test_minimization --seed 111 --only-faults=",
                passes=1,
                replays=1,
            ),
        }
        for seed, value in docs.items():
            (campaign / "minimization_reports" / f"{seed}.json").write_text(
                json.dumps(value), encoding="utf-8"
            )
        (campaign / "minimization_reports" / "108.json").write_text("{not json", encoding="utf-8")
        with (campaign / "minimization_reports" / "112.json").open("wb") as handle:
            handle.truncate(serve.MAX_MINIMIZATION_BYTES + 1)

        def write_small_campaign(
            name: str, paths: dict[str, str] | None = None, report: dict[str, Any] | None = None
        ) -> None:
            directory = cls.root / name
            (directory / "universes").mkdir(parents=True)
            (directory / "minimizations").mkdir()
            manifest = {
                "schema_version": 1,
                "campaign_id": name,
                "name": name,
                "complete": True,
                "build": {"id": f"{name}-build", "mode": "SIM"},
                "config": {"trials": 1},
            }
            if paths is not None:
                manifest["paths"] = paths
            (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            (directory / "campaign.json").write_text(
                json.dumps({"schema_version": 1, "campaign_id": name, "runs": 1}),
                encoding="utf-8",
            )
            trace_meta = {"present": False, "traced": False, "path": None, "hash": None}
            (directory / "universes.ndjson").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "index": 0,
                        "seed": "42",
                        "status": "failed",
                        "trace": trace_meta,
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            (directory / "universes" / "0.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "index": 0,
                        "seed": "42",
                        "seed_hex": f"0x{42:016x}",
                        "status": "failed",
                        "virtual_end_ns": 0,
                        "checks": [],
                        "trace": trace_meta,
                    }
                ),
                encoding="utf-8",
            )
            if report is not None:
                (directory / "minimizations" / "42.json").write_text(
                    json.dumps(report), encoding="utf-8"
                )

        # A manifest-declared directory outside the campaign must be a structured escape error,
        # not a silent "not minimized".
        write_small_campaign("escape", {"minimization_dir": "../escape-reports"})
        # A manifest without paths keeps the default minimizations directory.
        write_small_campaign("fallback", None, document(42))
        # A NUL in an untrusted manifest path is a structured error, not a generic 500.
        write_small_campaign("nul", {"minimization_dir": "minimizations\u0000x"})

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

    def request_json(self, seed: str):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request(
                "GET", f"/api/campaigns/minimization/universes/{seed}/minimization"
            )
            response = connection.getresponse()
            body = response.read()
            return response.status, json.loads(body.decode("utf-8"))
        finally:
            connection.close()

    def test_valid_report_is_returned_with_echoed_only_faults(self) -> None:
        status, payload = self.request_json("100")
        self.assertEqual(status, 200)
        report = payload["minimization"]
        self.assertTrue(report["present"])
        self.assertEqual(report["finding_id"], "double-oom")
        self.assertEqual([entry["seq"] for entry in report["candidates"]], [1, 2, 5])
        self.assertEqual([entry["seq"] for entry in report["kept"]], [2])
        self.assertEqual(report["only_faults"], [2])
        self.assertEqual(report["repro"], "test_minimization --seed 100 --only-faults=2")
        self.assertEqual(report["crashed_trials"], 0)

    def test_refused_report_carries_no_kept_set_or_repro(self) -> None:
        status, payload = self.request_json("102")
        self.assertEqual(status, 200)
        report = payload["minimization"]
        self.assertTrue(report["present"])
        self.assertTrue(report["refused"])
        self.assertIn("truncated", report["reason"])
        self.assertEqual(report["kept"], [])
        self.assertEqual(report["only_faults"], [])
        self.assertIsNone(report["repro"])

    def test_empty_kept_is_a_completed_report_not_a_missing_one(self) -> None:
        status, payload = self.request_json("111")
        self.assertEqual(status, 200)
        report = payload["minimization"]
        self.assertTrue(report["present"])
        self.assertFalse(report["refused"])
        self.assertEqual(report["kept"], [])
        self.assertEqual(report["candidates"], [])
        self.assertEqual(report["repro"], "test_minimization --seed 111 --only-faults=")

    def test_absent_report_is_present_false(self) -> None:
        status, payload = self.request_json("101")
        self.assertEqual(status, 200)
        self.assertFalse(payload["minimization"]["present"])

    def test_seed_mismatch_is_a_structured_error(self) -> None:
        status, payload = self.request_json("103")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_seed_mismatch")

    def test_only_faults_must_equal_the_kept_set(self) -> None:
        status, payload = self.request_json("104")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("only_faults", payload["error"]["message"])

    def test_kept_must_be_a_candidate(self) -> None:
        status, payload = self.request_json("105")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("not a candidate", payload["error"]["message"])

    def test_invalid_entry_status_is_rejected(self) -> None:
        status, payload = self.request_json("106")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("status", payload["error"]["message"])

    def test_unsupported_schema_is_rejected(self) -> None:
        status, payload = self.request_json("107")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "unsupported_schema")

    def test_malformed_json_is_a_structured_error(self) -> None:
        status, payload = self.request_json("108")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_malformed")

    def test_negative_counter_is_rejected(self) -> None:
        status, payload = self.request_json("109")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("passes", payload["error"]["message"])

    def test_crashed_trials_cannot_exceed_replays(self) -> None:
        status, payload = self.request_json("113")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("crashed_trials", payload["error"]["message"])

    def test_manifest_minimization_dir_is_honored(self) -> None:
        status, payload = self.request_json("100")
        self.assertEqual(status, 200)
        self.assertEqual(
            payload["minimization"]["path"], "minimization_reports/100.json"
        )

    def test_oversized_report_is_refused_before_read(self) -> None:
        status, payload = self.request_json("112")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_too_large")

    def test_huge_report_seed_is_a_structured_error(self) -> None:
        status, payload = self.request_json("110")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("64 bits", payload["error"]["message"])

    def test_non_monotonic_candidate_seq_is_rejected(self) -> None:
        status, payload = self.request_json("114")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("non-increasing seq", payload["error"]["message"])

    def test_empty_finding_id_is_rejected(self) -> None:
        status, payload = self.request_json("115")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")
        self.assertIn("finding_id", payload["error"]["message"])

    def test_manifest_minimization_dir_cannot_escape(self) -> None:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request(
                "GET", "/api/campaigns/escape/universes/42/minimization"
            )
            response = connection.getresponse()
            payload = json.loads(response.read().decode("utf-8"))
        finally:
            connection.close()
        self.assertEqual(response.status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_path_escape")

    def test_missing_manifest_paths_falls_back_to_default_dir(self) -> None:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request(
                "GET", "/api/campaigns/fallback/universes/42/minimization"
            )
            response = connection.getresponse()
            payload = json.loads(response.read().decode("utf-8"))
        finally:
            connection.close()
        self.assertEqual(response.status, 200)
        self.assertTrue(payload["minimization"]["present"])
        self.assertEqual(payload["minimization"]["path"], "minimizations/42.json")

    def test_nul_manifest_minimization_dir_is_a_structured_error(self) -> None:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", "/api/campaigns/nul/universes/42/minimization")
            response = connection.getresponse()
            payload = json.loads(response.read().decode("utf-8"))
        finally:
            connection.close()
        self.assertEqual(response.status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_minimization_malformed")




EVENT_HEADER = struct.Struct("<BBQQQQ")
EVENT_ENTRY = struct.Struct("<QqBqHIQQQQQQq")


def encode_events(entries, seed=42, dropped=0, rejected=0, source=3, version=1):
    data = EVENT_HEADER.pack(source, version, seed, dropped, rejected, len(entries))
    for entry in entries:
        data += EVENT_ENTRY.pack(*entry)
    return data


def event_entry(seq, at=0, kind=1, node=0xFFFFFFFF, task=0, correlation=0, subject=0,
                related=0, value=0, extra=0, amount=0, has_wall=0, wall=0):
    return (seq, at, has_wall, wall, kind, node, task, correlation, subject, related, value,
            extra, amount)


class EventArtifactCase(unittest.TestCase):
    """Synthetic event streams for the v1 codec's accepted and rejected shapes."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        campaign = cls.root / "events"
        (campaign / "universes").mkdir(parents=True)
        (campaign / "events").mkdir()
        (campaign / "manifest.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "campaign_id": "events",
                    "name": "Event variants",
                    "complete": True,
                    "build": {"id": "event-build", "mode": "SIM"},
                    "config": {"trials": 12, "events": True, "event_capacity": 4096},
                    "paths": {"event_dir": "events"},
                }
            ),
            encoding="utf-8",
        )
        (campaign / "campaign.json").write_text(
            json.dumps(
                {"schema_version": 1, "campaign_id": "events", "name": "Event variants",
                 "runs": 12}
            ),
            encoding="utf-8",
        )

        good = encode_events(
            [
                event_entry(0, at=0, kind=1, node=0, task=1, value=4),
                event_entry(1, at=100, kind=61, node=0, task=1, subject=2, value=1),
                event_entry(2, at=200, kind=26, node=0, task=1, correlation=7, subject=1000,
                            value=4, extra=4),
            ],
            seed=42,
        )
        gap = encode_events(
            [event_entry(2, at=200, kind=1), event_entry(3, at=300, kind=2)],
            seed=57,
            dropped=2,
            rejected=1,
        )
        variants = {
            0: ("42", True, "events/0.bin", good, None),
            1: ("43", True, "events/1.bin", None, None),
            2: ("44", False, "", b"", None),
            3: ("45", True, "events/3.bin", good, None),
            4: ("46", True, "events/4.bin",
                encode_events([event_entry(0, kind=1)], seed=46, source=2), None),
            5: ("47", True, "events/5.bin",
                encode_events([event_entry(0, kind=1)], seed=47, version=2), None),
            6: ("48", True, "events/6.bin", good[:-1], None),
            7: ("49", True, "events/7.bin", good + b"\x00", None),
            8: ("50", True, "events/8.bin",
                encode_events([], seed=50, dropped=1), None),
            9: ("51", True, "events/9.bin",
                encode_events([event_entry(1, kind=1), event_entry(3, kind=2)], seed=51), None),
            10: ("52", True, "events/10.bin",
                 encode_events([event_entry(0, kind=999)], seed=52), None),
            11: ("53", True, "events/11.bin",
                 encode_events([event_entry(0, kind=1, has_wall=2)], seed=53), None),
            12: ("54", True, "events/12.bin",
                 encode_events([event_entry(0, kind=1, has_wall=0, wall=5)], seed=54), None),
            13: ("55", True, "events/13.bin", good, None),
            14: ("56", True, "events/14.bin",
                 EVENT_HEADER.pack(3, 1, 56, 0, 0, serve.MAX_EVENT_ENTRIES + 1), None),
            15: ("57", True, "events/15.bin", gap, None),
            16: ("58", True, "events/16.bin", good, None),
        }
        # Variant 1 declares present but has no file; 3 has a mismatched path; 13/16 have a
        # mismatched seed in the stream header.
        path_by_index = {
            0: "events/0.bin", 1: "events/1.bin", 2: "",
            3: "events/999.bin", 4: "events/4.bin", 5: "events/5.bin", 6: "events/6.bin",
            7: "events/7.bin", 8: "events/8.bin", 9: "events/9.bin", 10: "events/10.bin",
            11: "events/11.bin", 12: "events/12.bin", 13: "events/13.bin",
            14: "events/14.bin", 15: "events/15.bin", 16: "events/16.bin",
        }
        if 3 in variants:
            variants[3] = ("45", True, "events/999.bin", good, None)
        if 13 in variants:
            variants[13] = ("55", True, "events/13.bin",
                            encode_events([event_entry(0, kind=1)], seed=99), None)
        if 16 in variants:
            variants[16] = ("58", True, "events/16.bin",
                            encode_events([event_entry(0, kind=1, task=2**64 - 1)],
                                          seed=58), None)

        rows = []
        for index, (seed, present, _declared, data, _unused) in variants.items():
            rows.append(
                {
                    "schema_version": 1,
                    "index": index,
                    "seed": seed,
                    "status": "passed",
                    "events": {
                        "present": present,
                        "attached": present,
                        "path": path_by_index[index],
                        "version": 1,
                        "capacity": 4096,
                        "dropped": 2 if index == 15 else 0,
                        "rejected": 1 if index == 15 else 0,
                    },
                }
            )
            if data:
                (campaign / "events" / f"{index}.bin").write_bytes(data)
            (campaign / "universes" / f"{index}.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "index": index,
                        "seed": seed,
                        "status": "passed",
                        "checks": [],
                        "events": rows[-1]["events"],
                    }
                ),
                encoding="utf-8",
            )
        (campaign / "universes.ndjson").write_text(
            "\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8"
        )

        cls.server = serve.create_server(cls.root, WEB_ROOT, "127.0.0.1", 0, quiet=True)
        cls.thread = threading.Thread(
            target=cls.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True
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
            return response.status, json.loads(response.read().decode("utf-8"))
        finally:
            connection.close()

    def test_gap_and_rejected_are_explicit(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/57/events")
        self.assertEqual(status, 200)
        events = payload["events"]
        self.assertEqual(events["dropped"], 2)
        self.assertEqual(events["rejected"], 1)
        self.assertEqual(events["gap"], {"count": 2, "first_seq": 2, "universe": "57"})
        self.assertEqual(events["first_seq"], 2)

    def test_missing_file_is_not_an_empty_success(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/43/events")
        self.assertEqual(status, 200)
        self.assertTrue(payload["events"]["missing"])
        self.assertEqual(payload["events"]["error"], "events_missing")
        self.assertEqual(payload["events"]["entries"], [])

    def test_absent_metadata_is_present_false(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/44/events")
        self.assertEqual(status, 200)
        self.assertFalse(payload["events"]["present"])
        self.assertEqual(payload["events"]["entries"], [])

    def test_path_mismatch_is_rejected(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/45/events")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_events_path_mismatch")

    def test_seed_mismatch_is_rejected(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/55/events")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_events_seed_mismatch")

    def test_malformed_streams_are_rejected(self) -> None:
        for seed, code in [
            ("46", "artifact_events_malformed"),
            ("47", "artifact_events_malformed"),
            ("48", "artifact_events_malformed"),
            ("49", "artifact_events_malformed"),
            ("50", "artifact_events_malformed"),
            ("51", "artifact_events_malformed"),
            ("52", "artifact_events_malformed"),
            ("53", "artifact_events_malformed"),
            ("54", "artifact_events_malformed"),
        ]:
            with self.subTest(seed=seed):
                status, payload = self.request(f"/api/campaigns/events/universes/{seed}/events")
                self.assertEqual(status, 500)
                self.assertEqual(payload["error"]["code"], code)

    def test_too_large_is_refused(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/56/events")
        self.assertEqual(status, 500)
        self.assertEqual(payload["error"]["code"], "artifact_events_too_large")

    def test_wide_reference_is_a_string(self) -> None:
        status, payload = self.request("/api/campaigns/events/universes/58/events")
        self.assertEqual(status, 200)
        self.assertEqual(payload["events"]["entries"][0]["task"], "18446744073709551615")

class ExplorationArtifactCase(unittest.TestCase):
    """Coverage/corpus and fork-COW branch documents are additive exploration artifacts."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        for campaign_id, with_exploration in (("explore", True), ("plain", False)):
            campaign = cls.root / campaign_id
            (campaign / "universes").mkdir(parents=True)
            (campaign / "manifest.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "campaign_id": campaign_id,
                        "name": campaign_id,
                        "complete": True,
                        "build": {"id": "explore-build", "mode": "SIM"},
                        "config": {"trials": 4, "trace": False},
                    }
                ),
                encoding="utf-8",
            )
            (campaign / "campaign.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "campaign_id": campaign_id,
                        "name": campaign_id,
                        "runs": 4,
                        "guidance": "coverage" if with_exploration else "seeded",
                        "coverage": {
                            "enabled": with_exploration,
                            "method": "trace-pc" if with_exploration else "",
                            "unique_edges": 9 if with_exploration else 0,
                            "corpus_size": 2 if with_exploration else 0,
                        },
                    }
                ),
                encoding="utf-8",
            )
            (campaign / "universes.ndjson").write_text("", encoding="utf-8")
            if with_exploration:
                (campaign / "exploration.json").write_text(
                    json.dumps(
                        {
                            "schema_version": 1,
                            "campaign_id": campaign_id,
                            "guidance": "coverage",
                            "coverage": {
                                "enabled": True,
                                "guidance": "coverage",
                                "method": "trace-pc",
                                "runs_with_coverage": 4,
                                "total_edges": 20,
                                "unique_edges": 9,
                                "novel_runs": 2,
                                "corpus_size": 2,
                                "corpus": [
                                    {
                                        "seed": "11",
                                        "parent_seed": None,
                                        "index": 0,
                                        "edges": 6,
                                        "new_edges": 6,
                                        "hash": "0x000000000000000b",
                                        "generation": 0,
                                        "finding": False,
                                    },
                                    {
                                        "seed": "12",
                                        "parent_seed": "11",
                                        "index": 1,
                                        "edges": 5,
                                        "new_edges": 3,
                                        "hash": "0x000000000000000c",
                                        "generation": 1,
                                        "finding": True,
                                    },
                                ],
                            },
                            "snapshots": [
                                {
                                    "id": 1,
                                    "parent_id": 0,
                                    "seed": "13",
                                    "at_ns": 0,
                                    "tasks": 3,
                                    "ready": 2,
                                    "blocked": 0,
                                    "ready_choices": 2,
                                    "heap_allocations": 0,
                                    "heap_bytes": 0,
                                    "endpoints": 0,
                                    "pending_packets": 0,
                                    "storage_files": 0,
                                    "trace_decisions": 0,
                                    "coverage_edges": 4,
                                    "coverage_hash": "0x000000000000000d",
                                    "method": "fork-cow",
                                }
                            ],
                            "branches": [
                                {
                                    "id": "S1-B0",
                                    "snapshot_id": 1,
                                    "branch_index": 0,
                                    "forced_choice": 0,
                                    "seed": "13",
                                    "status": "passed",
                                    "no_check_failed": True,
                                    "coverage": {"present": True, "edges": 4, "hash": "0x000000000000000e"},
                                    "trace": {"traced": False, "hash": None},
                                    "checks": [],
                                },
                                {
                                    "id": "S1-B1",
                                    "snapshot_id": 1,
                                    "branch_index": 1,
                                    "forced_choice": 1,
                                    "seed": "13",
                                    "status": "failed",
                                    "no_check_failed": False,
                                    "coverage": {"present": True, "edges": 4, "hash": "0x000000000000000f"},
                                    "trace": {"traced": False, "hash": None},
                                    "checks": [{"id": "order", "passed": False}],
                                },
                            ],
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

    def request(self, path: str):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            return response.status, json.loads(response.read().decode("utf-8"))
        finally:
            connection.close()

    def test_exploration_document_is_served(self) -> None:
        status, payload = self.request("/api/campaigns/explore/exploration")
        self.assertEqual(status, 200)
        self.assertTrue(payload["present"])
        self.assertEqual(payload["campaign_id"], "explore")
        self.assertEqual(payload["guidance"], "coverage")
        self.assertEqual(len(payload["coverage"]["corpus"]), 2)
        self.assertEqual(len(payload["snapshots"]), 1)
        self.assertEqual(len(payload["branches"]), 2)
        self.assertEqual(payload["branches"][1]["status"], "failed")

    def test_missing_exploration_is_explicit(self) -> None:
        status, payload = self.request("/api/campaigns/plain/exploration")
        self.assertEqual(status, 200)
        self.assertFalse(payload["present"])
        self.assertEqual(payload["campaign_id"], "plain")
        self.assertEqual(payload["snapshots"], [])
        self.assertEqual(payload["branches"], [])

    def test_malformed_exploration_is_rejected(self) -> None:
        path = self.root / "explore" / "exploration.json"
        original = json.loads(path.read_text())
        try:
            for mutation in ("campaign_id", "branch_status", "corpus_entry"):
                document = json.loads(json.dumps(original))
                if mutation == "campaign_id":
                    document["campaign_id"] = "other"
                elif mutation == "branch_status":
                    document["branches"][0]["status"] = "unknown"
                else:
                    document["coverage"]["corpus"][0] = "not-an-object"
                path.write_text(json.dumps(document), encoding="utf-8")
                status, payload = self.request("/api/campaigns/explore/exploration")
                self.assertEqual(status, 500)
                self.assertEqual(payload["error"]["code"], "artifact_exploration_malformed")
        finally:
            path.write_text(json.dumps(original), encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
