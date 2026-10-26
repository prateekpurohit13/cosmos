import { api } from "../lib/api.js";
import {
  badge,
  buildContext,
  codeBlock,
  copyButton,
  emptyState,
  kvList,
  notice,
  pageHeader,
  panel,
  reproIdentity,
  reproInvocation,
  seedLink,
  statGrid,
  statusBadge,
  timelineHref,
} from "../lib/components.js";
import { el, link } from "../lib/dom.js";
import {
  findingSeeds,
  firstFindingSeed,
  formatAt,
  formatCount,
  formatPercent,
  formatRatio,
  shortId,
} from "../lib/format.js";
import { setBuildContext } from "../lib/shell.js";
import { notFoundView } from "./not-found.js";

const SEED_PAGE_SIZE = 100;

function seedPageHref(page) {
  const params = new URLSearchParams(window.location.search);
  params.set("seed_page", String(page));
  return `${window.location.pathname}?${params.toString()}`;
}

function seedTable(finding, campaignId, indexes, manifest, page = 1) {
  const seeds = findingSeeds(finding);
  if (seeds.length === 0) {
    return emptyState({
      title: "No failing seeds exported",
      body: "This finding does not carry a seed list. The artifact schema-v1 contract expects one.",
    });
  }
  const ordered = seeds
    .map((seed) => ({ seed, row: indexes.get(seed) }))
    .sort((left, right) => {
      const leftIndex = left.row?.index;
      const rightIndex = right.row?.index;
      if (Number.isFinite(leftIndex) && Number.isFinite(rightIndex)) {
        if (leftIndex !== rightIndex) return leftIndex - rightIndex;
      } else if (Number.isFinite(leftIndex)) {
        return -1;
      } else if (Number.isFinite(rightIndex)) {
        return 1;
      }
      return left.seed < right.seed ? -1 : left.seed > right.seed ? 1 : 0;
    });

  const totalPages = Math.max(1, Math.ceil(ordered.length / SEED_PAGE_SIZE));
  const currentPage = Math.min(Math.max(1, Number(page) || 1), totalPages);
  const startIndex = (currentPage - 1) * SEED_PAGE_SIZE;
  const pageSeeds = ordered.slice(startIndex, startIndex + SEED_PAGE_SIZE);
  const table = el(
    "div",
    { class: "table-wrap seed-table" },
    el(
      "table",
      { class: "data data--seeds" },
      el(
        "caption",
        { class: "sr-only" },
        `Failing seeds page ${currentPage} of ${totalPages}`,
      ),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col", class: "num" }, "Index"),
          el("th", { scope: "col" }, "Seed"),
          el("th", { scope: "col" }, "Status"),
          el("th", { scope: "col", class: "num" }, "Checks"),
          el("th", { scope: "col" }, "Trace"),
          el("th", { scope: "col" }, "Replay"),
          el("th", { scope: "col" }, "Identity"),
        ),
      ),
      el(
        "tbody",
        {},
        ...pageSeeds.map(({ seed, row }) => {
          const checks = row?.checks ?? {};
          const invocation = reproInvocation(manifest, seed);
          const identity = reproIdentity(manifest, seed);
          return el(
            "tr",
            {},
            el(
              "td",
              { class: "num" },
              row
                ? link(
                    `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(seed)}`,
                    formatCount(row.index),
                  )
                : "—",
            ),
            el("td", {}, seedLink(campaignId, seed)),
            el("td", {}, row ? statusBadge(row.status) : el("span", { class: "faint" }, "not listed")),
            el(
              "td",
              { class: "num" },
              row ? formatRatio(checks.failed, checks.total) : "—",
            ),
            el(
              "td",
              {},
              row?.trace?.present
                ? el("code", { class: "mono", title: String(row.trace.hash ?? "") }, shortId(row.trace.hash))
                : el(
                    "span",
                    { class: "faint" },
                    !row ? "not listed" : row.trace?.traced ? "missing" : "not traced",
                  ),
            ),
            el(
              "td",
              {},
              row
                ? link(timelineHref(campaignId, seed, { checkId: finding.id, atNs: finding.at_ns }), "timeline", {
                    class: "btn btn--ghost btn--sm",
                    title: `Open the timeline at ${finding.id}`,
                  })
                : el("span", { class: "faint" }, "—"),
            ),
            el(
              "td",
              {},
              copyButton(invocation ?? identity, invocation ? "Copy" : "Copy id", {
                title: invocation ?? identity,
              }),
            ),
          );
        }),
      ),
    ),
  );
  if (ordered.length <= SEED_PAGE_SIZE) return table;
  return el(
    "div",
    {},
    table,
    el(
      "div",
      { class: "pager" },
      el(
        "span",
        {},
        `${formatCount(startIndex + 1)}–${formatCount(startIndex + pageSeeds.length)} of ` +
          `${formatCount(ordered.length)} seeds · page ${formatCount(currentPage)} of ` +
          `${formatCount(totalPages)}`,
      ),
      el(
        "div",
        { class: "pager__buttons" },
        currentPage > 1
          ? link(seedPageHref(currentPage - 1), "Previous", {
              class: "btn btn--ghost btn--sm",
            })
          : el(
              "span",
              { class: "btn btn--ghost btn--sm", "aria-disabled": "true" },
              "Previous",
            ),
        currentPage < totalPages
          ? link(seedPageHref(currentPage + 1), "Next", {
              class: "btn btn--ghost btn--sm",
            })
          : el(
              "span",
              { class: "btn btn--ghost btn--sm", "aria-disabled": "true" },
              "Next",
            ),
      ),
    ),
  );
}

export async function findingView({ campaignId, findingId, signal }) {
  const data = await api.campaign(campaignId, signal);
  const campaign = data.campaign ?? {};
  const manifest = data.manifest ?? {};
  const universes = Array.isArray(data.universes) ? data.universes : [];
  const findings = Array.isArray(campaign.findings)
    ? campaign.findings.filter((item) => item && typeof item === "object")
    : [];
  const finding = findings.find((item) => String(item.id) === String(findingId));
  if (!finding) {
    return notFoundView({
      title: "Finding not found",
      message: `Campaign ${campaignId} has no finding ${findingId}.`,
    });
  }
  setBuildContext(buildContext(manifest));

  const seeds = findingSeeds(finding);
  const firstSeed = firstFindingSeed(finding);
  const runs = Number(campaign.runs) || 0;
  const failed = Number(finding.universes_failed ?? seeds.length);
  const universesBySeed = new Map(universes.map((row) => [String(row.seed), row]));
  const seedIndex = seeds
    .map((seed) => universesBySeed.get(seed)?.index)
    .filter((index) => Number.isFinite(index));
  const firstIndex = seedIndex.length > 0 ? Math.min(...seedIndex) : undefined;
  const seedPage = Math.max(
    1,
    Number.parseInt(new URLSearchParams(window.location.search).get("seed_page") ?? "1", 10) || 1,
  );
  const build = manifest.build ?? {};
  const firstInvocation = firstSeed ? reproInvocation(manifest, firstSeed) : null;
  const firstIdentity = firstSeed ? reproIdentity(manifest, firstSeed) : null;

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId} / finding`,
    title: finding.id || "Finding",
    subtitle: finding.detail || "No detail was exported for this finding.",
    crumbs: [
      { label: "Campaigns", href: "/" },
      {
        label: campaign.name || campaignId,
        href: `/campaigns/${encodeURIComponent(campaignId)}`,
      },
      { label: finding.id || "Finding" },
    ],
    badges: [
      finding.rare ? badge("rare", "warn") : null,
      badge(`${formatCount(failed)} ${failed === 1 ? "universe" : "universes"}`, "bad"),
      firstIndex !== undefined ? badge(`first #${formatCount(firstIndex)}`, "plain") : null,
      el(
        "span",
        { class: "badge badge--plain" },
        formatAt(finding.at_ns),
      ),
    ].filter(Boolean),
    actions: [
      link(`/campaigns/${encodeURIComponent(campaignId)}`, "Campaign cockpit", {
        class: "btn btn--ghost",
      }),
      firstSeed
        ? link(
            timelineHref(campaignId, firstSeed, {
              checkId: finding.id,
              atNs: finding.at_ns,
            }),
            "Timeline at failure",
            { class: "btn btn--primary" },
          )
        : null,
      firstSeed
        ? copyButton(
            firstInvocation ?? firstIdentity,
            firstInvocation ? "Copy first repro" : "Copy repro identity",
            { class: "btn btn--ghost", title: firstInvocation ?? firstIdentity },
          )
        : null,
    ].filter(Boolean),
  });

  const stats = statGrid([
    { label: "Failed universes", value: formatCount(failed), tone: "bad" },
    {
      label: "Prevalence",
      value: formatPercent(failed, runs),
      note: `${formatCount(failed)} of ${formatCount(runs)} runs`,
      tone: "bad",
    },
    { label: "Exported seeds", value: formatCount(seeds.length), tone: "warn" },
    {
      label: "First universe",
      value: firstIndex !== undefined ? `#${formatCount(firstIndex)}` : "—",
      note: firstSeed || "no seed",
    },
    { label: "First failure time", value: formatAt(finding.at_ns), note: "virtual" },
    { label: "Build", value: build.id || "—", note: build.mode || "unknown mode" },
  ]);

  const ledger = panel({
    title: "Ledger",
    subtitle: "The first failing universe's decisions, as exported in the campaign report",
    body: finding.ledger
      ? codeBlock(finding.ledger, { wrap: true })
      : emptyState({
          title: "No ledger attached",
          body: "This finding does not carry a rendered ledger. Open a failing universe for its structured ledger.",
        }),
  });

  const seedsPanel = panel({
    title: "All failing seeds",
    subtitle: "Ordered by universe index when the artifact exposes one",
    count: seeds.length,
    body: seedTable(finding, campaignId, universesBySeed, manifest, seedPage),
    flush: true,
  });

  const reproPanel = panel({
    title: "Repro",
    subtitle: "Build-scoped identity; a command only when the artifact declares one",
    body: el(
      "div",
      {},
      firstSeed
        ? el(
            "div",
            {},
            el(
              "div",
              { class: "cluster" },
              el("code", { class: "inline-code repro-identity" }, firstInvocation ?? firstIdentity),
              copyButton(
                firstInvocation ?? firstIdentity,
                firstInvocation ? "Copy command" : "Copy repro identity",
              ),
            ),
            firstInvocation
              ? null
              : notice(
                  "info",
                  "No runnable repro template",
                  "This artifact does not declare a single-seed command. The copied identity names the program, build and seed; run the program's single-seed mode with this seed.",
                ),
          )
        : emptyState({
            title: "No seed to reproduce",
            body: "This finding has no exported failing seed.",
          }),
      kvList([
        ["Program", manifest.program || "—"],
        ["Build id", build.id || "—"],
        ["Commit", build.commit || "—"],
        ["Binary hash", build.binary_hash || "—"],
        ["Mode", build.mode || "—"],
      ]),
      notice(
        "info",
        "Causality is not claimed",
        "Findings are deduplicated by assertion id. The timeline opens at the nearest recorded event at or before a failed check; it does not claim that event caused the failure. Minimization (U2.4) and its 1-minimal label are not available yet.",
      ),
    ),
  });

  return {
    title: `${finding.id || "Finding"} · ${campaign.name || campaignId} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      stats,
      el("div", { class: "grid-2" }, ledger, reproPanel),
      el("div", { class: "section-gap" }, seedsPanel),
    ),
  };
}
