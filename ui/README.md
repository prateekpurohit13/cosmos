# Cosmos UI

A local, read-only campaign cockpit for the schema-v1 artifacts written by
`include/cosmos/export.hpp`. It is a separate process from the simulation binary (D4): no
simulation semantics, canonical trace encoding, or `trace_hash` are touched.

The v1 UI covers the campaign cockpit, universe explorer/detail, and finding triage. It does not
include a live event stream, timeline/replay controls, breakpoints, snapshots, or minimization
execution; those are later phases in `docs/ui-design.md`.

## Run it

```sh
nix develop -c just ui
```

Open <http://127.0.0.1:8000/>. The default artifact root is `ui/fixtures`, which contains the
checked-in `demo` campaign. To serve another artifact root:

```sh
nix develop -c just ui root=/path/to/artifacts port=9000
```

Or invoke the server directly (useful for `--host`, `--quiet`, or a non-default web root):

```sh
nix develop -c python3 ui/server/serve.py --root /path/to/artifacts --port 9000
```

The server binds to loopback with no authentication. A non-loopback bind is refused unless
`--allow-remote` is passed explicitly; do not expose it to a network you do not trust.

Regenerate the deterministic fixture if needed:

```sh
nix develop -c just ui-fixture
```

Two regenerations are byte-identical.

## Routes

| Path | View |
|---|---|
| `/` | Campaign list |
| `/campaigns/<campaign-id>` | Campaign cockpit |
| `/campaigns/<campaign-id>/universes` | Universe explorer |
| `/campaigns/<campaign-id>/universes/<seed>` | Universe detail |
| `/campaigns/<campaign-id>/findings/<assertion-id>` | Finding triage |

All routes are deep-linkable. Explorer filters live in the query string (`status`, `finding`,
`verify`, `q`, `page`) and use `history.replaceState`, so browser navigation still moves between
pages rather than filter states.

## API

The server exposes a small read-only JSON API over one artifact root:

| Endpoint | Response |
|---|---|
| `GET /api/campaigns` | `{schema_version, campaigns[]}` catalog |
| `GET /api/campaigns/<id>` | `{schema_version, campaign_id, manifest, campaign, universes[]}` |
| `GET /api/campaigns/<id>/universes` | `{schema_version, campaign_id, manifest, universes[]}` |
| `GET /api/campaigns/<id>/universes/<seed>` | `{schema_version, campaign_id, manifest, campaign (summary), universe}` |

`GET` and `HEAD` are supported. The server rejects unknown API routes, invalid campaign ids,
invalid seeds, non-loopback `Host` headers, and paths that resolve outside the artifact root.
Artifacts are validated as `schema_version: 1`; an unsupported or malformed artifact returns a
structured JSON error rather than guessing or dropping the connection.

Catalog entries carry `readable: true` for a loadable manifest and `readable: false` plus
`catalog_error` when the manifest cannot be read. A readable campaign with `complete: false` stays
openable; the cockpit shows its write error instead of hiding the artifact.

## Layout

```text
ui/
  README.md              this file
  web/                   vanilla ES modules, HTML and CSS; no build step
    app.js               shell, theme, router dispatch
    lib/                 DOM, API, formatting, fault metadata, components
    views/               campaign list, cockpit, universes, universe, finding, errors
  server/serve.py        Python 3 stdlib static + artifact API server
  server/test_serve.py   server contract tests
  fixtures/demo/         checked-in deterministic artifact
```

The server serves `ui/web` and falls back to `index.html` for `/campaigns/...` client routes. It
disables directory listings and does not serve dotfiles. Responses carry a restrictive
`Content-Security-Policy`, `nosniff`, and `no-store`; the frontend has no inline scripts or
styles.

## Tests

```sh
nix develop -c just ui-test
```

The suite covers the catalog, campaign/universe payloads, decimal and hex seed lookup, traversal
rejection, Host validation, NUL paths, non-finite/surrogate artifact values, SPA fallback, static
content types, HEAD behavior, security headers, and broken artifacts. `just test` runs it after
the C++ suite.

## Data rules the UI relies on

- Seeds and hashes are strings in the artifact. The UI never formats a seed through a numeric
  conversion, so full 64-bit identities stay visible and copyable.
- Virtual time is always labelled; wall durations are separate. The UI never puts wall time on
  the simulation axis.
- Findings, universes, sites and classes use the artifact's stable ordering, with explicit
  deterministic tie-breaks where a view sorts (findings by prevalence then assertion id; sites by
  slot).
- Vacuity is its own amber group with a reason token. It is not a finding, a pass, or a failed
  run.
- A repro always names the build identity. A seed alone is not presented as portable. The UI
  copies a build-scoped identity by default; it copies a runnable command only when the manifest
  supplies a `repro` template containing `{seed}` (optionally also `{program}`, `{campaign}`,
  `{build}`, `{commit}` or `{binary_hash}`). Unknown placeholders fall back to the identity.
- Trace truncation and absence are warnings, not silent gaps.

## v1 limits

- No live campaign ingestion; the server reads completed artifacts.
- No timeline, replay, compare, minimization execution, snapshots, or interactive control.
- The universe table is paginated at 100 rows and filters client-side. A future virtualization
  pass can replace pagination without changing the artifact contract.
- The API is local-only and unauthenticated by design. Add auth only as part of a separate
  deployment decision.

See `docs/ui-design.md` Appendix A for the frozen schema, decisions, task backlog, and handoff
context.
