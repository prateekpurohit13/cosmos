import { api } from "../lib/api.js";
import {
  badge,
  buildContext,
  emptyState,
  notice,
  pageHeader,
  panel,
  seedLink,
  statGrid,
} from "../lib/components.js";
import { el, link } from "../lib/dom.js";
import { formatAt, formatCount, formatNs, shortId } from "../lib/format.js";
import { navigate } from "../lib/router.js";
import { setBuildContext } from "../lib/shell.js";

function campaignHref(campaignId) {
  return `/campaigns/${encodeURIComponent(String(campaignId))}`;
}

function branchId(branch, index) {
  return String(branch?.id || `branch-${index}`);
}

function branchTone(status) {
  switch (status) {
    case "passed":
      return "ok";
    case "failed":
    case "crashed":
    case "threw":
    case "error":
      return "bad";
    case "vacuous":
      return "warn";
    default:
      return "neutral";
  }
}

function checkSummary(branch) {
  const checks = Array.isArray(branch?.checks) ? branch.checks : [];
  const failed = checks.filter((check) => check?.passed === false).length;
  return `${formatCount(failed)} failed / ${formatCount(checks.length)}`;
}

function coverageOf(branch) {
  const coverage = branch?.coverage ?? {};
  return coverage.present
    ? `${formatCount(coverage.edges)} edges`
    : "not captured";
}

function corpusTable(campaignId, corpus) {
  if (corpus.length === 0) {
    return emptyState({
      title: "Empty corpus",
      body: "Coverage was collected, but no run added a new edge.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data" },
      el(
        "caption",
        { class: "sr-only" },
        `Coverage corpus for ${campaignId}`,
      ),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Seed"),
          el("th", { scope: "col" }, "Parent"),
          el("th", { scope: "col", class: "num" }, "Edges"),
          el("th", { scope: "col", class: "num" }, "New"),
          el("th", { scope: "col", class: "num" }, "Gen"),
          el("th", { scope: "col" }, "Hash"),
          el("th", { scope: "col" }, "Result"),
        ),
      ),
      el(
        "tbody",
        {},
        ...corpus.map((entry) =>
          el(
            "tr",
            {},
            el("td", {}, seedLink(campaignId, String(entry.seed))),
            el(
              "td",
              { class: "mono" },
              entry.parent_seed ? seedLink(campaignId, String(entry.parent_seed)) : "initial",
            ),
            el("td", { class: "num" }, formatCount(entry.edges)),
            el("td", { class: "num" }, formatCount(entry.new_edges)),
            el("td", { class: "num" }, formatCount(entry.generation)),
            el("td", { class: "mono" }, shortId(entry.hash)),
            el(
              "td",
              {},
              entry.vacuous
        ? badge("vacuous", "warn")
        : entry.finding
          ? badge("finding", "bad")
          : badge("clean", "ok"),
            ),
          ),
        ),
      ),
    ),
  );
}

function snapshotCard(snapshot, branches) {
  const rows = branches.map((branch) =>
    el(
      "tr",
      {},
      el("td", { class: "mono" }, branchId(branch)),
      el("td", {}, `B${formatCount(branch.branch_index)}`),
      el(
        "td",
        {},
        branch.has_forced_choice
          ? `choice ${formatCount(branch.forced_choice)}${
              branch.forced_choice_applied ? "" : " (not applied)"
            }`
          : "schedule stream",
      ),
      el(
        "td",
        {},
        el("span", { class: `status status--${branchTone(branch.status)}` }, branch.status),
      ),
      el("td", {}, checkSummary(branch)),
      el(
        "td",
        {},
        coverageOf(branch),
        branch.faults_quiet ? " · faults quiet" : "",
      ),
      el("td", { class: "mono" }, String(branch.seed ?? "—")),
    ),
  );
  return el(
    "article",
    { class: "branch-snapshot" },
    el(
      "header",
      { class: "branch-snapshot__head" },
      el("h3", {}, `Snapshot ${formatCount(snapshot.id)}`),
      el(
        "div",
        { class: "badge-row" },
        badge(snapshot.method || "fork-cow", "info"),
        badge(`${formatCount(snapshot.ready_choices)} choices`, "plain"),
        badge(`${formatCount(snapshot.tasks)} tasks`, "plain"),
        snapshot.faults_quiet ? badge("faults quiet", "warn") : null,
        snapshot.coverage_present
          ? badge(`${formatCount(snapshot.coverage_edges)} snapshot edges`, "plain")
          : null,
        snapshot.parent_id
          ? badge(
              `child of S${formatCount(snapshot.parent_id)}/B${formatCount(
                snapshot.parent_branch_index,
              )}`,
              "plain",
            )
          : null,
      ),
    ),
    el(
      "p",
      { class: "branch-snapshot__meta" },
      `${formatAt(snapshot.at_ns)} · seed ${String(snapshot.seed)} · heap ${formatCount(
        snapshot.heap_allocations,
      )} alloc / ${formatCount(snapshot.heap_bytes)} B · ${formatCount(
        snapshot.endpoints,
      )} endpoints · ${formatCount(snapshot.storage_files)} files · ${formatCount(
        snapshot.trace_decisions,
      )} decisions`,
    ),
    rows.length === 0
      ? emptyState({
          title: "No branches",
          body: "This snapshot was captured, but no branch child was run from it.",
        })
      : el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data" },
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col" }, "Branch"),
            el("th", { scope: "col" }, "Index"),
            el("th", { scope: "col" }, "Choice"),
            el("th", { scope: "col" }, "Status"),
            el("th", { scope: "col" }, "Checks"),
            el("th", { scope: "col" }, "Coverage"),
            el("th", { scope: "col" }, "Seed"),
          ),
        ),
        el("tbody", {}, ...rows),
      ),
    ),
  );
}

function compareValue(value, kind = "text") {
  if (value === null || value === undefined || value === "") return "—";
  if (kind === "bool") return value ? "yes" : "no";
  if (kind === "count") return formatCount(value);
  if (kind === "ns") return formatNs(value);
  return String(value);
}

function branchComparePanel(campaignId, branches) {
  if (branches.length < 2) {
    return emptyState({
      title: "Not enough branches to compare",
      body: "A snapshot with two or more branches is needed for a side-by-side comparison.",
    });
  }
  const params = new URLSearchParams(window.location.search);
  const fallbackLeft = branchId(branches[0], 0);
  const fallbackRight = branchId(branches[1], 1);
  const requestedLeft = params.get("left");
  const requestedRight = params.get("right");
  const leftId = branches.some((branch, index) => branchId(branch, index) === requestedLeft)
    ? requestedLeft
    : fallbackLeft;
  const rightId = branches.some((branch, index) => branchId(branch, index) === requestedRight)
    ? requestedRight
    : fallbackRight;
  const left = branches.find((branch, index) => branchId(branch, index) === leftId);
  const right = branches.find((branch, index) => branchId(branch, index) === rightId);

  const setSelection = (side, value) => {
    const next = new URLSearchParams(window.location.search);
    next.set("left", side === "left" ? value : leftId);
    next.set("right", side === "right" ? value : rightId);
    navigate(`${campaignHref(campaignId)}/exploration?${next.toString()}`, { replace: true });
  };
  const selector = (side, selected) => {
    const select = el(
      "select",
      { class: "input input--sm", "aria-label": `${side} branch` },
      ...branches.map((branch, index) =>
        el(
          "option",
          { value: branchId(branch, index), ...(branchId(branch, index) === selected ? { selected: true } : {}) },
          branchId(branch, index),
        ),
      ),
    );
    select.addEventListener("change", () => setSelection(side, select.value));
    return select;
  };

  const rows = [
    ["Status", left?.status, right?.status],
    ["Checks", checkSummary(left), checkSummary(right)],
    ["Seed", left?.seed, right?.seed],
    ["Forced choice", left?.forced_choice, right?.forced_choice],
    ["Virtual end", left?.virtual_end_ns, right?.virtual_end_ns],
    ["Coverage edges", left?.coverage?.edges, right?.coverage?.edges],
    ["Coverage hash", left?.coverage?.hash, right?.coverage?.hash],
    ["Trace hash", left?.trace?.hash, right?.trace?.hash],
    ["Trace dropped", left?.trace?.dropped, right?.trace?.dropped],
    ["Error", left?.error, right?.error],
    ["Threw", left?.threw, right?.threw],
  ];
  return el(
    "div",
    {},
    notice(
      "info",
      "Branch observations",
      "Branches are separate forked children of the checkpoint they name. A differing trace hash or coverage hash is evidence, not a causal claim.",
    ),
    el("div", { class: "compare-selectors" }, selector("left", leftId), selector("right", rightId)),
    el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data" },
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col" }, "Field"),
            el("th", { scope: "col" }, leftId),
            el("th", { scope: "col" }, rightId),
          ),
        ),
        el(
          "tbody",
          {},
          ...rows.map(([label, leftValue, rightValue]) =>
            el(
              "tr",
              { class: leftValue !== rightValue ? "row--diff" : undefined },
              el("th", { scope: "row" }, label),
              el("td", {}, compareValue(leftValue, label === "Virtual end" ? "ns" : "text")),
              el("td", {}, compareValue(rightValue, label === "Virtual end" ? "ns" : "text")),
            ),
          ),
        ),
      ),
    ),
  );
}

export async function explorationView({ campaignId, signal }) {
  const [exploration, campaignData] = await Promise.all([
    api.exploration(campaignId, signal),
    api.campaign(campaignId, signal),
  ]);
  const manifest = campaignData.manifest ?? {};
  setBuildContext(buildContext(manifest));
  const title = `Exploration · ${campaignData.campaign?.name || campaignId}`;
  const coverage = exploration.coverage ?? {};
  const corpus = Array.isArray(coverage.corpus) ? coverage.corpus : [];
  const snapshots = Array.isArray(exploration.snapshots) ? exploration.snapshots : [];
  const branches = Array.isArray(exploration.branches) ? exploration.branches : [];

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId}`,
    title,
    subtitle:
      "Coverage-guided corpus and fork-COW snapshot branches. These are observations; the canonical trace remains the replay input.",
    badges: [
      badge(exploration.guidance || campaignData.campaign?.guidance || "seeded", "info"),
      coverage.enabled ? badge(`${formatCount(coverage.unique_edges)} unique edges`, "ok") : badge("no coverage", "neutral"),
      branches.length > 0 ? badge(`${formatCount(branches.length)} branches`, "plain") : null,
    ].filter(Boolean),
    actions: [
      link(`${campaignHref(campaignId)}/universes`, "Universe explorer", {
        class: "btn btn--primary",
      }),
      link(campaignHref(campaignId), "Campaign cockpit", { class: "btn btn--ghost" }),
    ],
    crumbs: [
      { label: "Dashboard", href: "/" },
      { label: campaignId, href: campaignHref(campaignId) },
      { label: "Exploration" },
    ],
  });

  if (!exploration.present) {
    return {
      title: `${title} · Cosmos`,
      node: el(
        "div",
        { class: "page" },
        header,
        panel({
          title: "No exploration artifact",
          body: emptyState({
            title: "Seeded campaign only",
            body: "This artifact has no coverage corpus or snapshot branches. Run the exploration exporter to add them.",
            action: link("/", "Back to campaigns", { class: "btn btn--ghost" }),
          }),
        }),
      ),
    };
  }

  const coverageStats = statGrid([
    {
      label: "Guidance",
      value: exploration.guidance || campaignData.campaign?.guidance || "seeded",
      note: coverage.method || "no compiler coverage",
      tone: coverage.enabled ? "accent" : "neutral",
    },
    { label: "Unique edges", value: formatCount(coverage.unique_edges), tone: "accent" },
    { label: "Runs with coverage", value: formatCount(coverage.runs_with_coverage) },
    {
      label: "Novel runs",
      value: formatCount(coverage.novel_runs),
      note: "runs that added an edge",
      tone: Number(coverage.novel_runs) > 0 ? "ok" : "neutral",
    },
    {
      label: "Corpus",
      value: formatCount(coverage.corpus_size),
      note:
        Number(coverage.corpus_dropped) > 0
          ? `${formatCount(coverage.corpus_dropped)} novel seeds dropped at the cap`
          : "bounded novelty set",
      tone: Number(coverage.corpus_dropped) > 0 ? "warn" : "neutral",
    },
    { label: "Snapshots", value: formatCount(snapshots.length) },
    { label: "Branches", value: formatCount(branches.length) },
    {
      label: "Branch findings",
      value: formatCount(
        branches.filter(
          (branch) => branch?.status !== "passed" && branch?.status !== "vacuous",
        ).length,
      ),
      tone: branches.some(
        (branch) => branch?.status !== "passed" && branch?.status !== "vacuous",
      )
        ? "bad"
        : "ok",
    },
  ]);

  const coveragePanel = panel({
    title: "Coverage corpus",
    subtitle: "Seeds retained because they added an edge to the union",
    count: corpus.length,
    body: corpusTable(campaignId, corpus),
    flush: corpus.length > 0,
  });

  const treePanel = panel({
    title: "Snapshot branch tree",
    subtitle: "One fork-COW checkpoint, one child process per branch",
    count: snapshots.length,
    body:
      snapshots.length === 0
        ? emptyState({
            title: "No snapshots",
            body: "This exploration artifact has coverage data but no captured snapshots.",
          })
        : el(
            "div",
            { class: "branch-tree" },
            ...snapshots.map((snapshot) =>
              snapshotCard(
                snapshot,
                branches.filter((branch) => Number(branch.snapshot_id) === Number(snapshot.id)),
              ),
            ),
          ),
  });

  return {
    title: `${title} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      coverageStats,
      el("div", { class: "section-gap" }, coveragePanel),
      el("div", { class: "section-gap" }, treePanel),
      panel({
        title: "Child-run comparison",
        subtitle: "Select two branches from this artifact",
        body: branchComparePanel(campaignId, branches),
        className: "section-gap",
      }),
    ),
  };
}
