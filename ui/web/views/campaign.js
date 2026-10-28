import { api } from "../lib/api.js";
import {
  artifactBadges,
  badge,
  classChip,
  copyButton,
  emptyState,
  findingLink,
  kvList,
  meter,
  notice,
  pageHeader,
  panel,
  reproIdentity,
  reproInvocation,
  seedLink,
  statGrid,
  timelineHref,
} from "../lib/components.js";
import { el, link } from "../lib/dom.js";
import {
  findingSeeds,
  firstFindingSeed,
  formatCount,
  formatPercent,
  formatRatio,
} from "../lib/format.js";
import {
  compareSites,
  isEventSite,
  siteClass,
  siteStatus,
  vacuityAdvice,
} from "../lib/faults.js";

function compareFindings(left, right, runs) {
  const denominator = Number(runs) > 0 ? Number(runs) : 1;
  const lp = Number(left?.universes_failed ?? 0) / denominator;
  const rp = Number(right?.universes_failed ?? 0) / denominator;
  if (lp !== rp) return rp - lp;
  const l = String(left?.id ?? "");
  const r = String(right?.id ?? "");
  return l < r ? -1 : l > r ? 1 : 0;
}

function siteMatrix(campaign) {
  const runs = Number(campaign.runs) || 0;
  const vacuousSites = new Set(
    (Array.isArray(campaign.vacuous) ? campaign.vacuous : []).map((site) =>
      String(site?.site ?? ""),
    ),
  );
  const sites = Object.entries(campaign.sites ?? {}).sort(([left], [right]) =>
    compareSites(left, right),
  );
  if (sites.length === 0) {
    return emptyState({
      title: "No site counters",
      body: "This campaign artifact does not carry the per-site activation matrix.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--sites" },
      el(
        "caption",
        { class: "sr-only" },
        `Site activation, eligibility and injection counters for ${formatCount(runs)} universes`,
      ),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Site"),
          el("th", { scope: "col" }, "Class"),
          el("th", { scope: "col", class: "num" }, "Activated"),
          el("th", { scope: "col", class: "num" }, "Eligible"),
          el("th", { scope: "col", class: "num" }, "Fired"),
          el("th", { scope: "col" }, "Status"),
        ),
      ),
      el(
        "tbody",
        {},
        ...sites.map(([site, row]) => {
          const activated = Number(row?.activated ?? 0);
          const eligible = Number(row?.eligible ?? 0);
          const fired = Number(row?.fired ?? 0);
          const eventSite = isEventSite(site);
          const state = eventSite
            ? {
                label: activated > 0 ? "armed" : "not activated",
                tone: activated > 0 ? "ok" : "neutral",
              }
            : siteStatus(activated, eligible, fired, vacuousSites.has(site));
          const counterTitle = eventSite
            ? "Episode site: starts and heals are recorded in the fault ledger and timeline, not in wrapper-call counters"
            : undefined;
          return el(
            "tr",
            {},
            el(
              "td",
              {},
              el("span", { class: "mono" }, site),
            ),
            el("td", {}, classChip(siteClass(site))),
            el(
              "td",
              {
                class: "num",
                title: `${formatCount(activated)} of ${formatCount(runs)} universes armed this site`,
              },
              formatRatio(activated, runs),
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

function vacuityBody(campaign) {
  const vacuous = Array.isArray(campaign.vacuous)
    ? campaign.vacuous.filter((site) => site && typeof site === "object")
    : [];
  if (vacuous.length === 0) {
    return emptyState({
      title: "No vacuous sites",
      body: "Every site the configuration armed was reached and fired at least once. This is separate from findings.",
    });
  }
  const reasons = new Map();
  for (const site of vacuous) {
    const reason = String(site.reason || "unknown");
    reasons.set(reason, (reasons.get(reason) ?? 0) + 1);
  }
  const summary = [...reasons.entries()]
    .map(([reason, count]) => `${formatCount(count)} ${reason.replace(/_/g, " ")}`)
    .join(" · ");
  return el(
    "div",
    {},
    el("p", { class: "vacuity-summary" }, summary),
    el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data" },
        el(
          "caption",
          { class: "sr-only" },
          "Sites the configuration armed that were never activated, reached or fired",
        ),
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col" }, "Site"),
            el("th", { scope: "col" }, "Reason"),
          ),
        ),
        el(
          "tbody",
          {},
          ...vacuous.map((site) =>
            el(
              "tr",
              {},
              el("td", {}, el("span", { class: "mono" }, site.site || "unknown")),
              el(
                "td",
                {},
                el("span", { class: "vacuity__reason" }, String(site.reason || "unknown")),
              ),
            ),
          ),
        ),
      ),
    ),
    el(
      "ul",
      { class: "vacuity-advice" },
      ...[...reasons.keys()].map((reason) =>
        el(
          "li",
          {},
          `${reason.replace(/_/g, " ")}: ${vacuityAdvice(reason)}`,
        ),
      ),
    ),
  );
}

function findingsBody(campaign, campaignId, manifest, seedIndex) {
  const runs = Number(campaign.runs) || 0;
  const findings = [...(campaign.findings ?? [])]
    .filter((finding) => finding && typeof finding === "object")
    .sort((left, right) => compareFindings(left, right, runs));
  if (findings.length === 0) {
    return emptyState({
      title: "No findings",
      body: "No assertion failed in this campaign. Check the vacuity group: a green campaign that armed nothing is not a clean one.",
    });
  }
  return el(
    "div",
    { class: "findings" },
    ...findings.map((finding) => {
      const seeds = findingSeeds(finding);
      const first = firstFindingSeed(finding);
      const firstIndex = seedIndex.get(first);
      const failed = Number(finding.universes_failed ?? 0);
      const invocation = first ? reproInvocation(manifest, first) : null;
      const identity = first ? reproIdentity(manifest, first) : null;
      return el(
        "article",
        { class: "finding" },
        el(
          "div",
          { class: "finding__head" },
          el(
            "h3",
            { class: "finding__id" },
            findingLink(campaignId, finding.id ?? "unknown"),
          ),
          el(
            "div",
            { class: "finding__meta" },
            finding.rare ? badge("rare", "warn") : null,
            seeds.length > 1 ? badge(`${formatCount(seeds.length)} seeds`, "plain") : null,
          ),
        ),
        finding.detail
          ? el("p", { class: "finding__detail" }, String(finding.detail))
          : null,
        el(
          "div",
          { class: "finding__prevalence" },
          el(
            "span",
            { class: "finding__prevalence-label" },
            formatPercent(failed, runs),
          ),
          el("div", { class: "meter-wrap" }, meter(failed, runs, "bad", "finding prevalence")),
          el(
            "span",
            { class: "finding__meta" },
            `${formatCount(failed)} of ${formatCount(runs)} universes`,
          ),
        ),
        seeds.length > 0
          ? el(
              "div",
              { class: "finding__seeds" },
              el("span", {}, "repro seeds"),
              ...seeds.slice(0, 2).map((seed) => seedLink(campaignId, seed, { class: "finding__seed" })),
              seeds.length > 2
                ? el(
                    "span",
                    { class: "faint" },
                    `+${formatCount(seeds.length - 2)} more`,
                  )
                : null,
            )
          : null,
        el(
          "div",
          { class: "finding__actions" },
          invocation
            ? copyButton(invocation, "Copy repro", { title: invocation })
            : identity
              ? copyButton(identity, "Copy repro identity", { title: identity })
              : null,
          first && firstIndex !== undefined
            ? link(
                timelineHref(campaignId, first, {
                  checkId: finding.id ?? null,
                  atNs: finding.at_ns,
                }),
                "Timeline at failure",
                { class: "btn btn--ghost btn--sm" },
              )
            : null,
          first && firstIndex !== undefined
            ? link(
                `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(first)}`,
                "Open first universe",
                { class: "btn btn--ghost btn--sm" },
              )
            : null,
          findingLink(campaignId, finding.id ?? "unknown", "Open finding", {
            class: "btn btn--ghost btn--sm",
          }),
        ),
      );
    }),
  );
}

export async function campaignView({ campaignId, signal }) {
  const explorationFetch = api
    .exploration(campaignId, signal)
    .then((exploration) => ({ exploration, error: null }))
    .catch((error) => (signal?.aborted ? Promise.reject(error) : { exploration: null, error }));
  const [data, loadedExploration] = await Promise.all([
    api.campaign(campaignId, signal),
    explorationFetch,
  ]);
  const exploration = loadedExploration.exploration ?? { present: false };
  const explorationError = loadedExploration.error;
  const manifest = data.manifest ?? {};
  const campaign = data.campaign ?? {};
  const universes = Array.isArray(data.universes) ? data.universes : [];
  const seedIndex = new Map(universes.map((row) => [String(row.seed), row.index]));
  const runs = Number(campaign.runs) || 0;
  const findings = Array.isArray(campaign.findings)
    ? campaign.findings.filter((finding) => finding && typeof finding === "object")
    : [];
  const vacuous = Array.isArray(campaign.vacuous)
    ? campaign.vacuous.filter((site) => site && typeof site === "object")
    : [];
  const neverHit = Array.isArray(campaign.never_hit) ? campaign.never_hit : [];
  const config = manifest.config ?? {};


  const badges = [
    ...artifactBadges(manifest),
    campaign.refused ? badge("refused", "bad") : null,
  ].filter(Boolean);

  const header = pageHeader({
    title: campaignId,
    badges,
    actions: [
      link(`/campaigns/${encodeURIComponent(campaignId)}/universes`, "Universe explorer", {
        class: "btn btn--primary",
      }),
    ],
  });

  const stats = statGrid(
    [
      { label: "Runs", value: formatCount(campaign.runs), tone: "accent" },
      {
        label: "Failed",
        value: formatCount(campaign.failed_runs),
        note: formatPercent(campaign.failed_runs, runs),
        tone: Number(campaign.failed_runs) > 0 ? "bad" : "ok",
      },
      Number(campaign.crashed_runs) > 0
        ? { label: "Crashed", value: formatCount(campaign.crashed_runs), tone: "bad" }
        : null,
      {
        label: "Findings",
        value: formatCount(findings.length),
        tone: findings.length > 0 ? "bad" : "ok",
      },
      neverHit.length > 0
        ? { label: "Never hit", value: formatCount(neverHit.length), tone: "warn" }
        : null,
      {
        label: "Vacuous sites",
        value: formatCount(vacuous.length),
        note: "separate from findings",
        tone: vacuous.length > 0 ? "warn" : "ok",
      },
      Number(campaign.out_of_scope_assertions) > 0
        ? {
            label: "Out of scope",
            value: formatCount(campaign.out_of_scope_assertions),
            tone: "warn",
          }
        : null,
    ].filter(Boolean),
  );

  const explorationPanel =
    exploration.present || campaign.coverage?.enabled || explorationError
      ? panel({
          title: "Exploration",
          subtitle: "Coverage corpus and fork-COW snapshot branches",
          count: Array.isArray(exploration.branches) ? exploration.branches.length : 0,
          actions: [
            link(`/campaigns/${encodeURIComponent(campaignId)}/exploration`, "Open exploration", {
              class: "btn btn--ghost btn--sm",
            }),
          ],
          body: el(
            "div",
            {},
            kvList([
              ["Guidance", campaign.guidance || exploration.guidance || "seeded"],
              [
                "Coverage method",
                campaign.coverage?.method || exploration.coverage?.method || "—",
              ],
              ["Unique edges", formatCount(campaign.coverage?.unique_edges ?? exploration.coverage?.unique_edges)],
              ["Corpus", formatCount(campaign.coverage?.corpus_size ?? exploration.coverage?.corpus_size)],
              ["Snapshots", formatCount((exploration.snapshots ?? []).length)],
              ["Branches", formatCount((exploration.branches ?? []).length)],
            ]),
            explorationError
              ? notice(
                  "bad",
                  "Exploration artifact could not be read",
                  explorationError.message || String(explorationError),
                )
              : exploration.present
                ? null
                : notice(
                    "info",
                    "No branch artifact",
                    "Coverage is present in the campaign report. Run the exploration exporter to add a snapshot branch tree.",
                  ),
          ),
        })
      : null;

  const findingsPanel = panel({
    title: "Findings",
    hint: "Deduplicated by assertion id, ordered by prevalence then id.",
    count: findings.length,
    body: findingsBody(campaign, campaignId, manifest, seedIndex),
  });

  const vacuityPanel = panel({
    title: "Vacuity",
    hint: "Configuration facts, never findings",
    count: vacuous.length,
    tone: "warn",
    body: vacuityBody(campaign),
  });

  const neverHitPanel = panel({
    title: "Never hit",
    subtitle: "Registered properties no universe reported as hit",
    count: neverHit.length,
    body:
      neverHit.length > 0
        ? el(
            "ul",
            { class: "never-hit" },
            ...neverHit.map((id) => el("li", {}, badge(String(id), "info"))),
          )
        : emptyState({
            title: "No never-hit properties",
            body: "Every property that was registered in this campaign was hit at least once.",
          }),
  });

  const configPanel = panel({
    title: "Launch configuration",
    hint: "Template, not the per-universe realized config",
    body: el(
      "div",
      {},
      kvList([
        ["Trials", formatCount(config.trials ?? campaign.runs)],
        ["Base seed", String(config.base_seed ?? campaign.base_seed_dec ?? "—")],
        ["Nodes", formatCount(config.node_count)],
        ["Verify", config.verify ? "double-run" : "off"],
      ]),
      el(
        "p",
        { class: "notice notice--info" },
        "Swarm sampling can vary the realized config per seed. Open a universe to see the config it actually ran.",
      ),
    ),
  });

  return {
    title: `${campaignId} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      stats,
      explorationPanel,
      el("div", { class: "cockpit" }, findingsPanel, vacuityPanel),
      panel({
        title: "Site matrix",
        subtitle: "Activated, eligible and fired across the campaign",
        body: siteMatrix(campaign),
        flush: true,
        className: "section-gap",
      }),
      el(
        "div",
        { class: "grid-2 section-gap" },
        neverHitPanel,
        configPanel,
      ),
    ),
  };
}
