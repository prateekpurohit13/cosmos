import { api } from "../lib/api.js";
import {
  badge,
  emptyState,
  pageHeader,
  panel,
  seedLink,
  statGrid,
  statusBadge,
} from "../lib/components.js";
import { button, el, link } from "../lib/dom.js";
import {
  formatCount,
  formatNs,
  formatPercent,
  formatWallNs,
  shortId,
} from "../lib/format.js";

const PAGE_SIZE = 100;
const STATUS_ORDER = ["failed", "crashed", "threw", "verify_failed", "vacuous", "passed"];

function compareStrings(left, right) {
  const l = String(left);
  const r = String(right);
  return l < r ? -1 : l > r ? 1 : 0;
}

function statusRank(status) {
  const index = STATUS_ORDER.indexOf(String(status));
  return index === -1 ? STATUS_ORDER.length : index;
}

function statusOptions(rows) {
  const seen = new Set(rows.map((row) => String(row.status ?? "unknown")));
  return [...seen].sort((left, right) => {
    const rank = statusRank(left) - statusRank(right);
    return rank !== 0 ? rank : compareStrings(left, right);
  });
}

function readState(params) {
  return {
    status: params.get("status") ?? "all",
    finding: params.get("finding") ?? "all",
    verify: params.get("verify") ?? "all",
    q: params.get("q") ?? "",
    page: Math.max(1, Number.parseInt(params.get("page") ?? "1", 10) || 1),
  };
}

function findingSeedSets(findings) {
  const map = new Map();
  for (const finding of findings) {
    if (!finding || typeof finding !== "object") continue;
    const seeds = (Array.isArray(finding.seeds) ? finding.seeds : []).filter(
      (seed) => typeof seed === "string",
    );
    if (seeds.length === 0 && typeof finding.seed_dec === "string") {
      seeds.push(finding.seed_dec);
    }
    map.set(String(finding.id), new Set(seeds));
  }
  return map;
}

function normalizeState(state, rows, findings) {
  const statuses = new Set(statusOptions(rows));
  if (state.status !== "all" && !statuses.has(state.status)) state.status = "all";
  const findingIds = new Set(findings.map((finding) => String(finding.id)));
  if (state.finding !== "all" && !findingIds.has(state.finding)) state.finding = "all";
  if (!["all", "ok", "failed", "off"].includes(state.verify)) state.verify = "all";
  return state;
}

function rowMatches(row, state, findingSeeds, campaign) {
  if (state.status !== "all" && String(row.status) !== state.status) return false;
  if (state.finding !== "all") {
    const seeds = findingSeeds.get(state.finding);
    if (!seeds || !seeds.has(String(row.seed))) return false;
  }
  if (state.verify === "failed" && !(row.verify_failed || row.status === "verify_failed")) {
    return false;
  }
  if (state.verify === "off" && campaign.verify) return false;
  if (state.verify === "ok") {
    if (!campaign.verify || row.verify_failed || row.status === "verify_failed") {
      return false;
    }
  }
  if (state.q) {
    const haystack = [
      row.index,
      row.seed,
      row.status,
      row.config_hash,
      row.trace?.hash,
      row.trace?.path,
    ]
      .filter((value) => value !== undefined && value !== null)
      .join(" ")
      .toLowerCase();
    if (!haystack.includes(state.q.toLowerCase())) return false;
  }
  return true;
}

function updateUrl(state) {
  const params = new URLSearchParams();
  if (state.status !== "all") params.set("status", state.status);
  if (state.finding !== "all") params.set("finding", state.finding);
  if (state.verify !== "all") params.set("verify", state.verify);
  if (state.q) params.set("q", state.q);
  if (state.page > 1) params.set("page", String(state.page));
  const query = params.toString();
  const path = window.location.pathname;
  window.history.replaceState(null, "", query ? `${path}?${query}` : path);
}

function th(label, className = "") {
  return el("th", { scope: "col", class: className || undefined }, label);
}

function traceCell(row) {
  const trace = row.trace ?? {};
  if (!trace.present) {
    return el(
      "span",
      { class: "faint" },
      trace.traced ? "missing" : "not traced",
    );
  }
  const cell = el(
    "span",
    { class: "cluster" },
    el("code", { class: "mono", title: String(trace.hash ?? "") }, shortId(trace.hash)),
  );
  if (Number(trace.dropped) > 0) {
    cell.append(badge(`${formatCount(trace.dropped)} dropped`, "warn"));
  }
  return cell;
}

function verifyCell(row, campaign) {
  if (!campaign.verify) return el("span", { class: "faint" }, "off");
  if (row.verify_failed || row.status === "verify_failed") {
    return statusBadge("verify_failed");
  }
  return el("span", { class: "status status--ok" }, "ok");
}

function universeTable(rows, campaign, campaignId) {
  if (rows.length === 0) {
    return emptyState({
      title: "No universes match these filters",
      body: "Reset the filters or try a different search term. The campaign artifact itself may also contain zero universes.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--universes" },
      el("caption", { class: "sr-only" }, `Universes in campaign ${campaignId}`),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          th("Index", "num"),
          th("Seed"),
          th("Status"),
          th("Virtual end", "num"),
          th("Wall", "num"),
          th("Checks", "num"),
          th("Fired / eligible", "num"),
          th("Active allocs", "num"),
          th("Trace"),
          th("Verify"),
          th("Config", "num"),
        ),
      ),
      el(
        "tbody",
        {},
        ...rows.map((row) => {
          const checks = row.checks ?? {};
          const failedChecks = Number(checks.failed ?? 0);
          return el(
            "tr",
            {},
            el(
              "td",
              { class: "num" },
              link(
                `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(row.seed)}`,
                formatCount(row.index),
              ),
            ),
            el("td", {}, seedLink(campaignId, row.seed)),
            el("td", {}, statusBadge(row.status)),
            el("td", { class: "num" }, formatNs(row.virtual_end_ns)),
            el("td", { class: "num" }, formatWallNs(row.wall_ns)),
            el(
              "td",
              { class: "num" },
              failedChecks > 0
                ? el(
                    "span",
                    { class: "status status--bad" },
                    `${formatCount(failedChecks)} / ${formatCount(checks.total)}`,
                  )
                : `${formatCount(checks.failed)} / ${formatCount(checks.total)}`,
            ),
            el(
              "td",
              { class: "num" },
              `${formatCount(row.fired)} / ${formatCount(row.eligible)}`,
            ),
            el("td", { class: "num" }, formatCount(row.active_allocations)),
            el("td", {}, traceCell(row)),
            el("td", {}, verifyCell(row, campaign)),
            el(
              "td",
              { class: "num" },
              el(
                "code",
                { class: "mono", title: String(row.config_hash ?? "") },
                shortId(row.config_hash),
              ),
            ),
          );
        }),
      ),
    ),
  );
}

function pager(state, totalPages, start, end, totalMatching, totalRows, onChange) {
  return el(
    "div",
    { class: "pager" },
    el(
      "span",
      {},
      `${formatCount(start)}–${formatCount(end)} of ${formatCount(totalMatching)} matching ` +
        `· ${formatCount(totalRows)} total · page ${formatCount(state.page)} of ${formatCount(totalPages)}`,
    ),
    el(
      "div",
      { class: "pager__buttons" },
      button("First", () => onChange(1), {
        class: "btn btn--ghost btn--sm",
        disabled: state.page <= 1,
        dataset: { pagerAction: "first" },
      }),
      button("Previous", () => onChange(state.page - 1), {
        class: "btn btn--ghost btn--sm",
        disabled: state.page <= 1,
        dataset: { pagerAction: "previous" },
      }),
      button("Next", () => onChange(state.page + 1), {
        class: "btn btn--ghost btn--sm",
        disabled: state.page >= totalPages,
        dataset: { pagerAction: "next" },
      }),
      button("Last", () => onChange(totalPages), {
        class: "btn btn--ghost btn--sm",
        disabled: state.page >= totalPages,
        dataset: { pagerAction: "last" },
      }),
    ),
  );
}

export async function universesView({ campaignId, signal }) {
  const data = await api.campaign(campaignId, signal);
  const campaign = data.campaign ?? {};
  const manifest = data.manifest ?? {};
  const rows = Array.isArray(data.universes) ? data.universes : [];
  const findings = Array.isArray(campaign.findings)
    ? campaign.findings.filter((finding) => finding && typeof finding === "object")
    : [];
  const seedsByFinding = findingSeedSets(findings);
  const state = normalizeState(readState(new URLSearchParams(window.location.search)), rows, findings);


  const statuses = statusOptions(rows);
  const statusSelect = el(
    "select",
    { id: "filter-status", name: "status" },
    el("option", { value: "all" }, `All statuses (${formatCount(rows.length)})`),
    ...statuses.map((status) =>
      el("option", { value: status }, `${status} (${formatCount(rows.filter((row) => row.status === status).length)})`),
    ),
  );
  statusSelect.value = state.status;

  const findingSelect = el(
    "select",
    { id: "filter-finding", name: "finding" },
    el("option", { value: "all" }, "All findings"),
    ...findings
      .slice()
      .sort((left, right) => compareStrings(left.id, right.id))
      .map((finding) =>
        el(
          "option",
          { value: String(finding.id) },
          `${finding.id} (${formatCount(finding.universes_failed)})`,
        ),
      ),
  );
  findingSelect.value = state.finding;

  const verifySelect = el(
    "select",
    { id: "filter-verify", name: "verify" },
    el("option", { value: "all" }, "All verify results"),
    el("option", { value: "ok" }, "Verify ok (campaign enabled)"),
    el("option", { value: "failed" }, "Verify mismatch"),
    el("option", { value: "off" }, "Verify off (campaign)"),
  );
  verifySelect.value = state.verify;

  const searchInput = el("input", {
    id: "filter-search",
    name: "q",
    type: "search",
    placeholder: "seed, index, status, hash…",
    autocomplete: "off",
    spellcheck: "false",
    value: state.q,
  });

  const summaryHost = el("div", {
    class: "filter-summary",
    role: "status",
    "aria-live": "polite",
    tabindex: "-1",
  });
  const resultsHost = el("div", {});
  let urlTimer = 0;

  const refresh = () => {
    const previousPager = document.activeElement?.closest?.("[data-pager-action]");
    const pagerAction = previousPager?.dataset.pagerAction;
    const filtered = rows.filter((row) => rowMatches(row, state, seedsByFinding, campaign));
    const totalPages = Math.max(1, Math.ceil(filtered.length / PAGE_SIZE));
    if (state.page > totalPages) state.page = totalPages;
    if (state.page < 1) state.page = 1;
    const startIndex = (state.page - 1) * PAGE_SIZE;
    const pageRows = filtered.slice(startIndex, startIndex + PAGE_SIZE);
    const end = startIndex + pageRows.length;
    const scheduledPath = window.location.pathname;
    window.clearTimeout(urlTimer);
    urlTimer = window.setTimeout(() => {
      if (window.location.pathname !== scheduledPath) return;
      try {
        updateUrl(state);
      } catch {
        // URL bookkeeping must never break rendering.
      }
    }, 200);
    summaryHost.textContent =
      `${formatCount(filtered.length)} matching of ${formatCount(rows.length)} universes` +
      (state.q ? ` · query ${JSON.stringify(state.q)}` : "") +
      (state.status !== "all" ? ` · status ${state.status}` : "") +
      (state.finding !== "all" ? ` · finding ${state.finding}` : "") +
      (state.verify !== "all" ? ` · verify ${state.verify}` : "");
    const children = [universeTable(pageRows, campaign, campaignId)];
    if (filtered.length > 0) {
      children.push(
        pager(
          state,
          totalPages,
          pageRows.length > 0 ? startIndex + 1 : 0,
          end,
          filtered.length,
          rows.length,
          (page) => {
            state.page = Math.min(Math.max(1, page), totalPages);
            refresh();
          },
        ),
      );
    }
    resultsHost.replaceChildren(...children);
    if (pagerAction) {
      const target = resultsHost.querySelector(`[data-pager-action="${pagerAction}"]`);
      if (target && !target.disabled) {
        target.focus();
      } else {
        summaryHost.focus();
      }
    }
  };

  const onFilterChange = () => {
    state.status = statusSelect.value;
    state.finding = findingSelect.value;
    state.verify = verifySelect.value;
    state.q = searchInput.value;
    state.page = 1;
    refresh();
  };

  const form = el(
    "form",
    {
      class: "filters",
      role: "search",
      onSubmit: (event) => {
        event.preventDefault();
        onFilterChange();
      },
    },
    el(
      "div",
      { class: "field" },
      el("label", { for: "filter-status" }, "Status"),
      statusSelect,
    ),
    el(
      "div",
      { class: "field" },
      el("label", { for: "filter-finding" }, "Finding"),
      findingSelect,
    ),
    el(
      "div",
      { class: "field" },
      el("label", { for: "filter-verify" }, "Verify"),
      verifySelect,
    ),
    el(
      "div",
      { class: "field field--grow" },
      el("label", { for: "filter-search" }, "Search"),
      searchInput,
    ),
    button(
      "Clear",
      () => {
        statusSelect.value = "all";
        findingSelect.value = "all";
        verifySelect.value = "all";
        searchInput.value = "";
        onFilterChange();
      },
      { class: "btn btn--ghost" },
    ),
  );

  for (const control of [statusSelect, findingSelect, verifySelect]) {
    control.addEventListener("change", onFilterChange);
  }
  searchInput.addEventListener("input", onFilterChange);

  const failed = rows.filter((row) => row.status === "failed").length;
  const crashed = rows.filter((row) => row.status === "crashed" || row.status === "threw").length;
  const vacuous = rows.filter((row) => row.status === "vacuous").length;
  const verifyFailed = rows.filter(
    (row) => row.verify_failed || row.status === "verify_failed",
  ).length;
  const traceMissing = rows.filter(
    (row) => row.trace?.traced && !row.trace?.present,
  ).length;
  const traceNotCaptured = rows.filter((row) => !row.trace?.traced).length;

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId} / universes`,
    title: "Universe explorer",
    subtitle:
      campaign.name ||
      "Every row is one replayable universe. Filters are client-side and the page is deep-linkable.",
    crumbs: [
      { label: "Campaigns", href: "/" },
      {
        label: campaign.name || campaignId,
        href: `/campaigns/${encodeURIComponent(campaignId)}`,
      },
      { label: "Universes" },
    ],
    actions: [
      link(`/campaigns/${encodeURIComponent(campaignId)}`, "Campaign cockpit", {
        class: "btn btn--primary",
      }),
    ],
  });

  const stats = statGrid([
    { label: "Universes", value: formatCount(rows.length), tone: "accent" },
    { label: "Failed", value: formatCount(failed), tone: failed ? "bad" : "ok" },
    { label: "Crashed / threw", value: formatCount(crashed), tone: crashed ? "bad" : "neutral" },
    { label: "Vacuous", value: formatCount(vacuous), tone: vacuous ? "warn" : "neutral" },
    {
      label: "Verify mismatch",
      value: formatCount(verifyFailed),
      tone: verifyFailed ? "verify" : "neutral",
    },
    {
      label: "Trace missing",
      value: formatCount(traceMissing),
      note: traceMissing
        ? "traced but absent"
        : traceNotCaptured
          ? "traces were off"
          : "all traced",
      tone: traceMissing ? "warn" : traceNotCaptured ? "neutral" : "ok",
    },
    {
      label: "Prevalence",
      value: formatPercent(failed, rows.length),
      note: "failed / universes",
      tone: failed ? "bad" : "ok",
    },
    {
      label: "Configs",
      value: formatCount(new Set(rows.map((row) => row.config_hash)).size),
      note: "distinct realized hashes",
    },
  ]);

  refresh();

  return {
    title: `Universes · ${campaign.name || campaignId} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      stats,
      panel({
        title: "Universes",
        subtitle: "Ordered by universe index. The table is paginated; filters stay in the URL.",
        body: el("div", {}, form, summaryHost, resultsHost),
        flush: true,
      }),
    ),
  };
}
