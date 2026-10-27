import { api } from "../lib/api.js";
import {
  badge,
  buildContext,
  classChip,
  codeBlock,
  compareHref,
  copyButton,
  detailsBlock,
  emptyState,
  findingLink,
  kvList,
  minimizationHref,
  notice,
  pageHeader,
  panel,
  reproIdentity,
  reproInvocation,
  statGrid,
  statusBadge,
  timelineHref,
} from "../lib/components.js";
import { el, link } from "../lib/dom.js";
import {
  formatAt,
  formatCount,
  formatNs,
  formatWallNs,
  shortId,
} from "../lib/format.js";
import { compareSites, isEventSite, siteClass, siteStatus } from "../lib/faults.js";
import { finiteTimeNs } from "../lib/trace.js";
import { setBuildContext } from "../lib/shell.js";

const LEDGER_UI_LIMIT = 500;

function sumSiteCounters(sites, key) {
  return Object.values(sites ?? {}).reduce(
    (total, row) => total + Number(row?.[key] ?? 0),
    0,
  );
}

function ledgerStatus(status) {
  const token = String(status ?? "").toLowerCase();
  if (token === "fired") return "ok";
  if (token === "suppressed") return "verify";
  if (token === "skipped") return "warn";
  return "neutral";
}

function ledgerSiteCell(entry) {
  return el(
    "span",
    { class: "cluster" },
    el("span", { class: "mono" }, entry.site ?? "unknown"),
    classChip(entry.fault_class ?? "Unknown"),
  );
}

function ledgerBody(universe) {
  const ledger = (Array.isArray(universe.ledger) ? universe.ledger : []).filter(
    (entry) => entry && typeof entry === "object",
  );
  if (ledger.length === 0) {
    if (universe.ledger_text) {
      return el(
        "div",
        {},
        emptyState({
          title: "Structured ledger missing",
          body: "The artifact carries only the rendered ledger text.",
        }),
        codeBlock(universe.ledger_text, { wrap: true }),
      );
    }
    return emptyState({
      title: "No fault decisions recorded",
      body: "The injector either never ran or this artifact did not capture its ledger.",
    });
  }
  const shown = ledger.slice(0, LEDGER_UI_LIMIT);
  const table = el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--ledger" },
      el("caption", { class: "sr-only" }, "Structured fault decision ledger"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col", class: "num" }, "#"),
          el("th", { scope: "col", class: "num" }, "Virtual time"),
          el("th", { scope: "col" }, "Site"),
          el("th", { scope: "col" }, "Outcome"),
          el("th", { scope: "col" }, "Status"),
          el("th", { scope: "col" }, "Drew"),
          el("th", { scope: "col", class: "num" }, "Eligible"),
          el("th", { scope: "col" }, "Reason"),
          el("th", { scope: "col" }, "Persistent"),
        ),
      ),
      el(
        "tbody",
        {},
        ...shown.map((entry) =>
          el(
            "tr",
            {},
            el("td", { class: "num" }, formatCount(entry.fire_index)),
            el("td", { class: "num" }, formatAt(entry.at_ns)),
            el("td", {}, ledgerSiteCell(entry)),
            el("td", {}, el("span", { class: "mono" }, entry.outcome ?? "—")),
            el(
              "td",
              {},
              el(
                "span",
                { class: `status status--${ledgerStatus(entry.status)}` },
                entry.status ?? "—",
              ),
            ),
            el("td", {}, entry.drew ? "yes" : "no"),
            el("td", { class: "num" }, formatCount(entry.eligible_index)),
            el("td", {}, el("span", { class: "faint" }, entry.reason ?? "—")),
            el("td", {}, entry.persistent ? "yes" : "no"),
          ),
        ),
      ),
    ),
  );
  if (ledger.length <= LEDGER_UI_LIMIT) return table;
  return el(
    "div",
    {},
    table,
    notice(
      "info",
      "Ledger truncated by the UI",
      `Showing the first ${formatCount(LEDGER_UI_LIMIT)} of ${formatCount(ledger.length)} entries. ` +
        "The artifact still contains all rows; the trace timeline pages canonical decisions separately.",
    ),
  );
}

function rawDetails(universe) {
  const pre = codeBlock("");
  let loaded = false;
  return el(
    "details",
    {
      class: "details",
      onToggle: (event) => {
        if (event.currentTarget.open && !loaded) {
          pre.textContent = JSON.stringify(universe, null, 2);
          loaded = true;
        }
      },
    },
    el("summary", {}, "Universe JSON"),
    pre,
  );
}

function checksBody(universe, campaignId, seedValue, findingIds) {
  const checks = Array.isArray(universe.checks) ? universe.checks : [];
  if (checks.length === 0) {
    return emptyState({
      title: "No checks recorded",
      body: "This universe report has no check results.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--checks" },
      el("caption", { class: "sr-only" }, "Checks in this universe"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Check"),
          el("th", { scope: "col" }, "Result"),
          el("th", { scope: "col", class: "num" }, "Virtual time"),
          el("th", { scope: "col" }, "Detail"),
        ),
      ),
      el(
        "tbody",
        {},
        ...checks.map((check) => {
          const checkAt = finiteTimeNs(check.at_ns);
          const failed = check.passed === false;
          const result = check.passed === true ? "passed" : failed ? "failed" : "unknown";
          const tone = check.passed === true ? "ok" : failed ? "bad" : "neutral";
          return el(
            "tr",
            {},
            el(
              "td",
              {},
              findingIds.has(String(check.id))
                ? findingLink(campaignId, check.id)
                : el("span", { class: "mono" }, check.id ?? "—"),
            ),
            el(
              "td",
              {},
              el("span", { class: `status status--${tone}` }, result),
            ),
            el(
              "td",
              { class: "num" },
              checkAt !== null
                ? link(
                    timelineHref(campaignId, seedValue, {
                      checkId: failed ? check.id : null,
                      atNs: checkAt,
                    }),
                    formatAt(checkAt),
                    {
                      class: "mono link--subtle",
                      title: failed
                        ? "Open the timeline at this failed check"
                        : "Open the timeline at this check time",
                    },
                  )
                : formatAt(check.at_ns),
            ),
            el("td", { class: "text-soft" }, check.detail || "—"),
          );
        }),
      ),
    ),
  );
}

function coverageBody(universe) {
  const coverage = (Array.isArray(universe.coverage) ? universe.coverage : []).filter(
    (note) => note && typeof note === "object",
  );
  if (coverage.length === 0) {
    return emptyState({
      title: "No coverage notes",
      body: "This universe report has no coverage notes.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data" },
      el("caption", { class: "sr-only" }, "Coverage notes in this universe"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Property"),
          el("th", { scope: "col" }, "Hit"),
        ),
      ),
      el(
        "tbody",
        {},
        ...coverage.map((note) =>
          el(
            "tr",
            {},
            el("td", {}, el("span", { class: "mono" }, note.id ?? "—")),
            el(
              "td",
              {},
              note.hit
                ? el("span", { class: "status status--ok" }, "hit")
                : el("span", { class: "status status--neutral" }, "not hit"),
            ),
          ),
        ),
      ),
    ),
  );
}

function sitesBody(universe) {
  const sites = Object.entries(universe.sites ?? {}).sort(([left], [right]) =>
    compareSites(left, right),
  );
  if (sites.length === 0) {
    return emptyState({
      title: "No site counters",
      body: "This universe report does not carry per-site counters.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--sites" },
      el("caption", { class: "sr-only" }, "Per-site counters in this universe"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Site"),
          el("th", { scope: "col" }, "Class"),
          el("th", { scope: "col" }, "Activated"),
          el("th", { scope: "col", class: "num" }, "Eligible"),
          el("th", { scope: "col", class: "num" }, "Fired"),
          el("th", { scope: "col" }, "Status"),
        ),
      ),
      el(
        "tbody",
        {},
        ...sites.map(([site, row]) => {
          const activated = Boolean(row?.activated);
          const eligible = Number(row?.eligible ?? 0);
          const fired = Number(row?.fired ?? 0);
          const eventSite = isEventSite(site);
          const state = eventSite
            ? {
                label: activated ? "armed" : "not activated",
                tone: activated ? "ok" : "neutral",
              }
            : siteStatus(activated, eligible, fired);
          const counterTitle = eventSite
            ? "Episode site: starts and heals are recorded in the fault ledger and timeline, not in wrapper-call counters"
            : undefined;
          return el(
            "tr",
            {},
            el("td", {}, el("span", { class: "mono" }, site)),
            el("td", {}, classChip(siteClass(site))),
            el(
              "td",
              {},
              activated
                ? el("span", { class: "status status--ok" }, "yes")
                : el("span", { class: "status status--warn" }, "no"),
            ),
            el("td", { class: "num", title: counterTitle }, eventSite ? "—" : formatCount(eligible)),
            el("td", { class: "num", title: counterTitle }, eventSite ? "—" : formatCount(fired)),
            el(
              "td",
              {},
              el(
                "span",
                { class: `status status--${state.tone}`, title: counterTitle },
                state.label,
              ),
            ),
          );
        }),
      ),
    ),
  );
}

export async function universeView({ campaignId, seed, signal }) {
  const universeData = await api.universe(campaignId, seed, signal);
  const campaign = universeData.campaign ?? {};
  const manifest = universeData.manifest ?? {};
  const universe = universeData.universe ?? {};
  const findingIds = new Set(
    (Array.isArray(campaign.findings) ? campaign.findings : [])
      .filter((finding) => finding && typeof finding === "object")
      .map((finding) => String(finding.id)),
  );
  setBuildContext(buildContext(manifest));

  const index = universe.index ?? "—";
  const status = universe.status ?? "unknown";
  const checks = (Array.isArray(universe.checks) ? universe.checks : []).filter(
    (check) => check && typeof check === "object",
  );
  const failedChecks = checks.filter((check) => check.passed === false);
  const firstFailedCheck =
    failedChecks.find((check) => finiteTimeNs(check.at_ns) !== null) ?? failedChecks[0];
  const eligible = sumSiteCounters(universe.sites, "eligible");
  const fired = sumSiteCounters(universe.sites, "fired");
  const trace = universe.trace ?? {};
  const seedValue = universe.seed ?? seed;
  const invocation = reproInvocation(manifest, seedValue);
  const identity = reproIdentity(manifest, seedValue);
  const campaignFindings = Array.isArray(campaign.findings)
    ? campaign.findings.filter((finding) => finding && typeof finding === "object")
    : [];
  const findingsSeed = campaignFindings.find((finding) => {
    const seeds = Array.isArray(finding.seeds) ? finding.seeds : [];
    const wanted = String(universe.seed ?? seed);
    return seeds.some((item) => typeof item === "string" && item === wanted);
  });

  const badges = [
    statusBadge(status),
    findingsSeed ? badge(String(findingsSeed.id), "bad") : null,
    universe.verify_failed ? badge("verify mismatch", "verify") : null,
    universe.died ? badge(`signal ${formatCount(universe.signal)}`, "bad") : null,
    universe.threw ? badge("threw", "bad") : null,
  ].filter(Boolean);

  const subtitle = el(
    "span",
    {},
    "seed ",
    el("code", { class: "inline-code" }, String(universe.seed ?? seed)),
    universe.seed_hex ? el("span", { class: "faint" }, ` · ${universe.seed_hex}`) : null,
  );

  const notices = [];
  if (!trace.present) {
    notices.push(
      notice(
        "warn",
        "No canonical trace",
        trace.traced
          ? "The trace file is missing from this artifact; replay evidence is unavailable."
          : "This campaign did not capture canonical trace bytes for this universe.",
      ),
    );
  }
  if (Number(trace.dropped) > 0) {
    notices.push(
      notice(
        "warn",
        "Trace evidence truncated",
        `The trace buffer dropped ${formatCount(trace.dropped)} entries; the record is incomplete.`,
      ),
    );
  }
  if (universe.verify_failed || universe.verify_mismatch) {
    notices.push(
      notice(
        "verify",
        "Verification failed",
        universe.verify_mismatch ||
          "Two runs of this seed did not agree. Treat the run as unverified evidence.",
      ),
    );
  }
  if (universe.vacuous) {
    notices.push(
      notice(
        "warn",
        "Vacuous universe",
        "This universe registered no checks, so it verified nothing. Vacuity is not a finding.",
      ),
    );
  }
  if (universe.died) {
    notices.push(
      notice(
        "bad",
        "Universe crashed",
        `The process died with signal ${formatCount(universe.signal)}. The report is incomplete.`,
      ),
    );
  }
  if (universe.threw) {
    notices.push(notice("bad", "Universe threw", universe.threw));
  }

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId} / universe`,
    title: `Universe #${formatCount(index)}`,
    subtitle,
    crumbs: [
      { label: "Campaigns", href: "/" },
      {
        label: campaign.name || campaignId,
        href: `/campaigns/${encodeURIComponent(campaignId)}`,
      },
      {
        label: "Universes",
        href: `/campaigns/${encodeURIComponent(campaignId)}/universes`,
      },
      { label: `#${formatCount(index)}` },
    ],
    badges,
    actions: [
      link(
        timelineHref(campaignId, seedValue, {
          checkId: firstFailedCheck ? firstFailedCheck.id : null,
          atNs: firstFailedCheck ? firstFailedCheck.at_ns : null,
        }),
        firstFailedCheck ? "Open timeline at failure" : "Open timeline",
        { class: "btn btn--primary" },
      ),
      link(`/campaigns/${encodeURIComponent(campaignId)}/universes`, "Back to universes", {
        class: "btn btn--ghost",
      }),
      link(compareHref(campaignId, seedValue), "Compare", {
        class: "btn btn--ghost",
        title: "Compare this universe with another universe in this campaign",
      }),
      link(
        `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(
          seedValue,
        )}/state`,
        "State inspector",
        {
          class: "btn btn--ghost",
          title: "Inspect the captured scheduler, network, storage, memory and RNG state",
        },
      ),
      link(minimizationHref(campaignId, seedValue), "Minimization", {
        class: "btn btn--ghost",
        title: "Open the exported MinimizeReport, or its explicit absence",
      }),
      copyButton(
        invocation ?? identity,
        invocation ? "Copy repro" : "Copy repro identity",
        { class: "btn btn--ghost", title: invocation ?? identity },
      ),
    ],
  });

  const stats = statGrid([
    {
      label: "Virtual end",
      value: formatNs(universe.virtual_end_ns),
      note: "simulation time",
      tone: "accent",
    },
    { label: "Wall duration", value: formatWallNs(universe.wall_ns), note: "host time" },
    {
      label: "Checks",
      value: `${formatCount(failedChecks.length)} / ${formatCount(checks.length)}`,
      note: "failed / total",
      tone: failedChecks.length > 0 ? "bad" : "ok",
    },
    {
      label: "Wrapper faults",
      value: `${formatCount(fired)} / ${formatCount(eligible)}`,
      note: "fired / eligible calls",
      tone: fired > 0 ? "warn" : "neutral",
    },
    {
      label: "Active allocations",
      value: formatCount(universe.active_allocations),
      tone: Number(universe.active_allocations) > 0 ? "warn" : "ok",
    },
    {
      label: "Out of scope",
      value: formatCount(universe.out_of_scope_assertions),
      tone: Number(universe.out_of_scope_assertions) > 0 ? "warn" : "neutral",
    },
    {
      label: "Config hash",
      value: shortId(universe.config_hash),
      note: universe.config ? "realized config captured" : "no config",
    },
    {
      label: "Trace",
      value: trace.present ? shortId(trace.hash) : trace.traced ? "missing" : "not traced",
      note: trace.present
        ? Number.isInteger(trace.dropped)
          ? trace.capacity
            ? `capacity ${formatCount(trace.capacity)}`
            : "no canonical bytes"
          : "drop count not declared"
        : "no canonical bytes",
      tone: trace.present && Number.isInteger(trace.dropped) && trace.dropped === 0 ? "ok" : "warn",
    },
  ]);

  const reproPanel = panel({
    title: "Repro",
    subtitle: "Build-scoped identity; a command only when the artifact declares one",
    body: el(
      "div",
      {},
      kvList([
        ["Program", manifest.program || "—"],
        ["Seed", String(seedValue)],
        ["Seed hex", universe.seed_hex || "—"],
        ["Build id", manifest.build?.id || "—"],
        ["Commit", manifest.build?.commit || "—"],
      ]),
      el(
        "div",
        { class: "cluster section-gap" },
        el("code", { class: "inline-code repro-identity" }, invocation ?? identity),
        copyButton(invocation ?? identity, invocation ? "Copy command" : "Copy repro identity"),
      ),
      invocation
        ? null
        : notice(
            "info",
            "No runnable repro template",
            "This artifact does not declare a single-seed command. The copied identity names the program, build and seed; run the program's single-seed mode with this seed.",
          ),
      trace.present
        ? el(
            "p",
            { class: "notice notice--info" },
            "Canonical trace: ",
            el("code", { class: "inline-code" }, trace.path || "—"),
            ` · hash ${trace.hash || "—"} · capacity ${formatCount(trace.capacity)}`,
          )
        : null,
    ),
  });

  const configPanel = panel({
    title: "Realized configuration",
    subtitle: "The injector config this universe actually ran",
    body: universe.config
      ? el(
          "div",
          {},
          kvList([
            ["Config hash", universe.config_hash || "—"],
            ["Mode", universe.config.mode || "—"],
            [
              "Enabled classes",
              Array.isArray(universe.config.enabled_classes)
                ? universe.config.enabled_classes.join(", ")
                : "—",
            ],
            [
              "Activated sites",
              Array.isArray(universe.config.activated_sites)
                ? universe.config.activated_sites.join(", ") || "none"
                : "—",
            ],
            ["Warmup", formatNs(universe.config.warmup_ns)],
            [
              "Quiesce after",
              universe.config.quiesce_after_ns === null ||
              universe.config.quiesce_after_ns === undefined
                ? "never"
                : formatNs(universe.config.quiesce_after_ns),
            ],
          ]),
          detailsBlock(
            "Realized config JSON",
            codeBlock(JSON.stringify(universe.config, null, 2)),
          ),
    )
      : emptyState({
          title: "No realized config",
          body: "This universe did not capture an injector configuration object.",
        }),
  });

  const rawPanel = panel({
    title: "Raw artifact",
    subtitle: "The per-universe schema-v1 document",
    body: rawDetails(universe),
  });

  return {
    title: `Universe #${formatCount(index)} · ${campaign.name || campaignId} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      stats,
      notices.length > 0 ? el("div", { class: "stack" }, ...notices) : null,
      el(
        "div",
        { class: "grid-2 section-gap" },
        panel({
          title: "Checks",
          subtitle: "Assertions evaluated in this universe",
          count: checks.length,
          body: checksBody(universe, campaignId, seedValue, findingIds),
        }),
        panel({
          title: "Coverage",
          subtitle: "Coverage notes reported by the application",
          count: universe.coverage?.length ?? 0,
          body: coverageBody(universe),
        }),
      ),
      panel({
        title: "Fault ledger",
        subtitle: "Canonical decision rows in decision order",
        count: universe.ledger?.length ?? 0,
        className: "section-gap",
        body: el(
          "div",
          {},
          Number(universe.ledger_dropped) > 0
            ? notice(
                "warn",
                "Ledger truncated",
                `${formatCount(universe.ledger_dropped)} ledger entries were dropped.`,
              )
            : null,
          ledgerBody(universe),
        ),
      }),
      panel({
        title: "Sites",
        subtitle: "Per-site counters captured at quiesce",
        className: "section-gap",
        body: sitesBody(universe),
      }),
      el(
        "div",
        { class: "grid-2 section-gap" },
        reproPanel,
        configPanel,
      ),
      el("div", { class: "section-gap" }, rawPanel),
    ),
  };
}
