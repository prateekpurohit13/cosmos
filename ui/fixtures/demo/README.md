# Demo campaign fixture

A tiny v1 artifact directory (`schema_version: 1`) used by the UI smoke tests. It is deliberately
small: four universes, one finding, two passes, and a 24-entry canonical trace per universe
spanning Memory and Clock lanes.

Regenerate it deterministically:

```sh
nix develop -c just ui-fixture
```

The exporter writes the artifact; `tools/normalise_ui_fixture.py` fixes the created/completed
timestamps, zeroes `wall_ns` in `universes/*.json` and `universes.ndjson`, and pins the compiler
field. `complete: true` therefore stays consistent with the writer's invariant (`complete` implies a
finish timestamp), while the fixture remains reviewable and reproducible.

The layout is documented in `docs/ui-design.md` Appendix A. The canonical trace files are the
same bytes `trace_hash()` covers, not a UI-specific format.
