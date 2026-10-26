# Cosmos UI

A local, read-only campaign cockpit for the schema-v1 artifacts written by
`include/cosmos/export.hpp`. It is a separate process from the simulation binary (D4): no
simulation semantics, canonical trace encoding, or `trace_hash` are touched.

The v3 UI covers the campaign cockpit, universe explorer/detail, finding triage, the canonical
fault timeline, deterministic replay controls, the finding → failed-check jump, two-universe
compare, the exported minimization report, and the read-only state inspector over the versioned
inspection stream. It does not include live streaming from a running campaign, minimization
execution, snapshots or arbitrary state mutation. Breakpoint inspection runs in the separate
single-universe `cosmos_inspect` process, not in the browser.

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
| `/campaigns/<campaign-id>/universes/<seed>/timeline` | Fault timeline |
| `/campaigns/<campaign-id>/universes/<seed>/minimization` | Exported minimization report |
| `/campaigns/<campaign-id>/universes/<seed>/state` | State inspector and inspection event stream |
| `/campaigns/<campaign-id>/compare?left=<seed>&right=<seed>` | Two-universe compare |
| `/campaigns/<campaign-id>/findings/<assertion-id>` | Finding triage |

All routes are deep-linkable. Explorer filters live in the query string (`status`, `finding`,
`verify`, `q`, `page`). Timeline search and selection live in `q` and `event`. Compare keeps
`left` and `right` in the query string. All three use `history.replaceState`, so browser navigation
still moves between pages rather than filter or selection states.

Timeline links may also carry transient `check=<assertion-id>` and `at=<virtual-time>` hints. The
view resolves them to the nearest recorded event at or before the check/time, then replaces the
URL with the canonical `event=<seq>` form. A finding or universe check time therefore opens the
timeline at the failure evidence instead of the timeline start. When the failure precedes the
first recorded event, the first event is selected and labelled; when it follows the last, the
last event is selected and labelled.

Compare accepts a decimal or `0x`-prefixed seed on either side. It resolves both against the
campaign's universe rows, canonicalizes the URL to the rows' decimal seed strings, and falls back
with a visible warning if a link names an invalid or unknown seed. Selecting a seed only changes
the URL; no artifact or universe is modified.

## API

The server exposes a small read-only JSON API over one artifact root:

| Endpoint | Response |
|---|---|
| `GET /api/campaigns` | `{schema_version, campaigns[]}` catalog |
| `GET /api/campaigns/<id>` | `{schema_version, campaign_id, manifest, campaign, universes[]}` |
| `GET /api/campaigns/<id>/universes` | `{schema_version, campaign_id, manifest, universes[]}` |
| `GET /api/campaigns/<id>/universes/<seed>` | `{schema_version, campaign_id, manifest, campaign (summary), universe}` |
| `GET /api/campaigns/<id>/universes/<seed>/trace` | `{schema_version, campaign_id, manifest, universe (summary), trace}` |
| `GET /api/campaigns/<id>/universes/<seed>/minimization` | `{schema_version, campaign_id, manifest, universe (summary), minimization}` |
| `GET /api/campaigns/<id>/universes/<seed>/events` | `{schema_version, campaign_id, manifest, universe (summary), events}` |

The trace response decodes `traces/<index>.bin` into the frozen `TraceEntry` fields and adds
`count`, `size_bytes`, `computed_hash`, `declared_hash`, `hash_matches`, `seed_matches` and the
decoded `entries[]`. A missing file returns `present: false, missing: true` so the timeline can
show the gap; malformed bytes, a bad source, truncation, trailing bytes or a non-canonical path
return a structured `artifact_trace_malformed`/`trace_path_mismatch` error. A hash or header-seed
mismatch is reported with the entries rather than hiding evidence. To keep the browser view
bounded, a trace that declares more than 100,000 entries or whose file exceeds the corresponding
~4.3 MB (4.1 MiB) bound returns `artifact_trace_too_large` instead of materializing an unbounded array. An
entry whose `seq`, eligible index, virtual time or amount is outside JavaScript's safe-integer
range is rejected as `artifact_trace_malformed` rather than rounded. Entry `seq` values must be
strictly increasing because `(universe, seq)` is the event identity.

`GET` and `HEAD` are supported. The server rejects unknown API routes, invalid campaign ids,
invalid seeds, non-loopback `Host` headers, and paths that resolve outside the artifact root.
Artifacts are validated as `schema_version: 1`; an unsupported or malformed artifact returns a
structured JSON error rather than guessing or dropping the connection.

The events endpoint decodes `events/<index>.bin` (source 3, version 1). It validates the source
and version, the declared/actual entry count, contiguous `seq` values, known kind ids, the wall
presence flag, the path and the stream universe. `entries[]` carries the decoded envelope plus
`kind`/`domain` tokens and string-valued payload fields so 64-bit identities survive JSON. The
response reports `present`, `attached`, `missing`, `capacity`, `dropped`, `rejected`, `gap`,
`first_seq` and `next_seq`. A malformed stream is `artifact_events_malformed`; a stream that
declares more than 100,000 entries is `artifact_events_too_large`; a path that disagrees with the
universe index is `artifact_events_path_mismatch`; a header universe that disagrees with the row is
`artifact_events_seed_mismatch`. A missing file is `present: false, missing: true` with an
`events_missing` code, so the view can distinguish lost evidence from an absent capture. A mutant
event stream never feeds replay.

The minimization endpoint reads the optional `minimizations/<seed>.json` document after resolving
the universe row. Absence is `present: false` with empty arrays and a null `repro`, never an error
and never an empty success. A report whose seed disagrees with the universe is
`artifact_minimization_seed_mismatch`; a malformed entry, non-monotonic seq, kept seq that is not a
candidate, or `only_faults` that is not exactly the kept set is
`artifact_minimization_malformed`; a refused report carrying a kept set or repro is also
malformed. The endpoint refuses documents larger than 8 MiB or with more than 100,000 candidates
(`artifact_minimization_too_large`) and never runs minimization or writes an artifact. It reads the
directory named by `manifest.paths.minimization_dir` and falls back to `minimizations`.

Catalog entries carry `readable: true` for a loadable manifest and `readable: false` plus
`catalog_error` when the manifest cannot be read. A readable campaign with `complete: false` stays
openable; the cockpit shows its write error instead of hiding the artifact.

## Layout

```text
ui/
  README.md              this file
  web/                   vanilla ES modules, HTML and CSS; no build step
    app.js               shell, theme, router dispatch, view teardown
    lib/                 DOM, API, formatting, fault metadata, components, trace, timeline, playback, compare
    views/               campaign list, cockpit, universes, universe, timeline, finding, minimization, state, compare, errors
  server/serve.py        Python 3 stdlib static + artifact API + canonical trace and event decoders
  server/test_serve.py   server contract tests, including trace, event and minimization variants
  fixtures/demo/         checked-in deterministic artifact with 24-entry traces, events and state
  ../tools/cosmos_inspect.cpp  interactive single-universe breakpoint inspector
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
rejection, Host validation, NUL paths, non-finite/surrogate artifact values, SPA fallback
(including the compare route), static content types, HEAD behavior, security headers, broken
artifacts, canonical trace decoding, multi-entry ordering, hash/seed mismatch reporting, missing
trace files, malformed source/truncated/trailing trace bytes, event source/version/count/
sequence/kind/wall/gap/seed/path/oversize variants, and minimization
present/refused/empty/missing/seed-mismatch/only-faults/entry-schema/oversize variants. `just test`
runs it after the C++ suite.

## Data rules the UI relies on

- Seeds and hashes are strings in the artifact. The UI never formats a seed through a numeric
  conversion, so full 64-bit identities stay visible and copyable. Time, duration, sequence and
  eligible-call fields remain JSON numbers per schema v1; the view is defined for values inside
  JavaScript's safe-integer range.
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
- The timeline decodes the canonical bytes exactly; it never re-encodes them or adds UI-only
  events to the canonical trace. A hash or header-seed mismatch is shown with the decoded entries.
- The table distinguishes a `FIRED` decision with `outcome: None` from an actual injection. The
  canonical trace has no reason, node, task or correlation field; the inspector shows an exact
  ledger match when one exists and labels the rest absent rather than inventing it.
- Playback is a read-only cursor over recorded entries. It is paced in events per second
  (`1`, `2`, `4`, `8`, `16`, `32`), never advances virtual time, and never mutates a universe.
  `Play`/`Pause`, step, next injected fault, next suppressed/skipped decision, next failed check
  and jump-to-virtual-time all land on canonical event boundaries. `Space`, `.`, `,`, `f`, `d`,
  `x`, `g` and `/` are the keyboard equivalents; `Esc` pauses playback. Search dims
  non-matching events rather than hiding them. With no selection, step selects the first event
  and step back selects the last; playback stops at the last event instead of silently rewinding.
  During playback the canvas cursor is coalesced to about 10 paints per second while the
  inspector and table keep up with every event.
- Compare is also read-only. It loads two universe documents and two canonical traces through the
  existing APIs. Canonical entries are compared pairwise in frozen encoding order; report fields
  use one fixed order; the realized config objects are diffed key by key with a bounded recursion
  and an explicit truncation notice. Wall duration is displayed as host metadata but is never
  compared, sorted or used to define the first divergence. Trace hashes are identity, not a
  divergence signal: the canonical header includes the universe seed, so equal decisions across
  different seeds still hash differently. The view marks the first event-pair difference, the first
  virtual-time difference and the first report/config field explicitly. Missing, malformed, seed-mismatched, truncated or drop-count-undeclared traces disable event
  comparison with a labelled explanation instead of treating absent evidence as equality. The fixed report order is: status; virtual end; report flags
  (`no_check_failed`, `vacuous`, `verify_failed`, `verify_mismatch`, `died`, `signal`, `threw`);
  `active_allocations`; `out_of_scope_assertions`; `ledger_dropped`; `config_hash`; trace metadata
  (`present`, `traced`, `dropped`); checks (totals, then each assertion's `passed`/`at_ns`/`detail`
  sorted by id); coverage hits by id; sites (`activated`/`eligible`/`fired`) in slot order; ledger
  row count and the first differing row in the recorded prefix (`ledger_dropped` is shown
  separately). Wall time, `seed` and trace hashes are excluded.
- Minimization is report-only (O5). The view reads the exported `MinimizeReport`; it never runs
  the minimizer, changes a universe or rewrites a trace. The guarantee is **1-minimal, not
  globally minimal**: every kept fire is necessary unless its removal trial crashed or exited
  without a verdict, and the set is not proven smallest. `crashed_trials` counts signal
  terminations only. Missing
  reports (`present: false`), refused reports (with their reason) and empty kept sets (the finding
  reproduces with every fault withheld) are three distinct states, never merged into success. The
  repro command is copied from the report; the UI does not synthesize `--only-faults` flags, and
  the build identity is shown separately. The copied command is the report's harness invocation;
  the viewer does not verify that the named program accepts those flags.
- A failed check has no event in the canonical trace. The timeline draws a dashed red marker at
  the nearest recorded event at or before `checks[].at_ns`, groups multiple checks that resolve
  to the same event, and labels `before first recorded event` / `after last recorded event` when
  the failure falls outside the trace span. It never invents a check event or claims causality.
- Inspection events are a separate versioned stream. The UI renders the explicit `dropped` gap
  and `rejected` count before the retained window; it never presents a truncated capture as the
  whole universe. Payloads are numeric and by-reference only; application names come from the
  state document. The stream is never used for replay, minimization or `trace_hash`.
- Breakpoints and commands belong to the single-universe inspector. A pause does not pop a ready
  task, so resuming observes the same scheduler order. A crash/reboot/pause/resume or partition
  command is applied at its recorded virtual time and emitted as `operator.command`; the command
  log is the reproducible record, and an unapplied command is reported rather than hidden.
- Search dims non-matching events instead of hiding them, so causal context stays visible.
  Supported fields are `site`, `class`, `outcome`, `status`, `seq` (or `#5`), `eligible`,
  `t`/`at`, `drew`/`drew:yes`/`drew:no`, `amount`, free text and a leading `-` for negation.
  Numeric field matches are exact (`t:0` is exactly zero virtual time), while free text is a
  case-insensitive substring.

## v3 limits

- No live campaign ingestion; the server reads completed artifacts.
- No minimization execution, snapshots, branching or arbitrary state mutation. Browser playback
  and the state inspector are read-only. The separate `cosmos_inspect` tool can pause at a
  scheduler boundary and records operator commands; it is not a browser control channel.
- The artifact layout holds one minimization report per seed: if a seed fails several assertions,
  the first finding in report order is minimized and the writer rejects a second report for that
  seed. The finding view labels a report whose `finding_id` differs instead of showing it as that
  finding.
- The universe, trace, candidate and kept tables are paginated at 100 rows; the universe and
  trace filters are client-side. The compare view
  compares the two selected universes only; it does not align more than two traces or build a
  branch tree. The trace API refuses more than 100,000 entries or ~4.3 MB (4.1 MiB) rather than
  freezing the tab; a paged trace API can replace that ceiling without changing the canonical
  trace format. The minimization API refuses reports larger than 8 MiB or with more than 100,000
  candidates.
- The API is local-only and unauthenticated by design. Add auth only as part of a separate
  deployment decision.

See `docs/ui-design.md` Appendix A for the frozen schema, decisions, task backlog, and handoff
context.
