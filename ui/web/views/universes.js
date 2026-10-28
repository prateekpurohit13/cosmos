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
  formatPercent,
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
  return state;
}

function rowMatches(row, state, findingSeeds) {
  if (state.status !== "all" && String(row.status) !== state.status) return false;
  if (state.finding !== "all") {
    const seeds = findingSeeds.get(state.finding);
    if (!seeds || !seeds.has(String(row.seed))) return false;
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
  if (state.q) params.set("q", state.q);
  if (state.page > 1) params.set("page", String(state.page));
  const query = params.toString();
  const path = window.location.pathname;
  window.history.replaceState(null, "", query ? `${path}?${query}` : path);
}

function th(label, className = "", hint) {
  return el("th", { scope: "col", class: className || undefined, ...(hint ? { title: hint } : {}) }, label);
}

// A trace is expected for every universe when the campaign traced, so a missing or truncated one is
// an integrity gap worth flagging inline. Present hashes are opaque and live on the compare page.
function traceWarning(row) {
  const trace = row.trace ?? {};
  if (trace.traced && !trace.present) return badge("no trace", "warn");
  if (Number(trace.dropped) > 0) return badge(`${formatCount(trace.dropped)} dropped`, "warn");
  return null;
}

function universeTable(rows, campaignId) {
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
          th("Index", "num", "Universe index within the campaign; links to the universe"),
          th("Seed", "", "Seed the universe ran with; links to the universe"),
          th("Status", "", "Outcome of the universe: passed, failed, crashed, threw, vacuous"),
          th("Checks", "num", "Failed checks / total checks"),
          th("Fired / eligible", "num", "Fault injections fired / fault sites eligible to fire"),
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
            el(
              "td",
              {},
              el("div", { class: "cluster" }, statusBadge(row.status), traceWarning(row)),
            ),
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
      `${formatCount(start)}–${formatCount(end)} of ${formatCount(totalMatching)}` +
        (totalMatching === totalRows ? "" : ` matching (${formatCount(totalRows)} total)`) +
        ` · page ${formatCount(state.page)} of ${formatCount(totalPages)}`,
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

  const searchInput = el("input", {
    id: "filter-search",
    name: "q",
    type: "search",
    placeholder: "seed, index, status, finding…",
    autocomplete: "off",
    spellcheck: "false",
    value: state.q,
  });

  const summaryHost = el("div", {
    class: "filter-summary sr-only",
    role: "status",
    "aria-live": "polite",
    tabindex: "-1",
  });
  const resultsHost = el("div", {});
  let urlTimer = 0;

  const refresh = () => {
    const previousPager = document.activeElement?.closest?.("[data-pager-action]");
    const pagerAction = previousPager?.dataset.pagerAction;
    const filtered = rows.filter((row) => rowMatches(row, state, seedsByFinding));
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
      (state.finding !== "all" ? ` · finding ${state.finding}` : "");
    const children = [universeTable(pageRows, campaignId)];
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
      { class: "field field--grow" },
      el("label", { for: "filter-search" }, "Search"),
      searchInput,
    ),
    button(
      "Clear",
      () => {
        statusSelect.value = "all";
        findingSelect.value = "all";
        searchInput.value = "";
        onFilterChange();
      },
      { class: "btn btn--ghost" },
    ),
  );

  for (const control of [statusSelect, findingSelect]) {
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

  const header = pageHeader({
    title: "Universe explorer",
    crumbs: [
      { label: "Campaigns", href: "/" },
      {
        label: campaignId,
        href: `/campaigns/${encodeURIComponent(campaignId)}`,
      },
      { label: "Universes" },
    ],
    actions: [
      link(`/campaigns/${encodeURIComponent(campaignId)}`, "Campaign", {
        class: "btn btn--primary",
      }),
    ],
  });

  const stats = statGrid([
    { label: "Universes", value: formatCount(rows.length), tone: "accent" },
    {
      label: "Failed",
      value: formatCount(failed),
      note: formatPercent(failed, rows.length),
      tone: failed ? "bad" : "ok",
    },
    crashed > 0
      ? { label: "Crashed / threw", value: formatCount(crashed), tone: "bad" }
      : null,
    vacuous > 0 ? { label: "Vacuous", value: formatCount(vacuous), tone: "warn" } : null,
    verifyFailed > 0
      ? { label: "Verify mismatch", value: formatCount(verifyFailed), tone: "verify" }
      : null,
    traceMissing > 0
      ? {
          label: "Trace missing",
          value: formatCount(traceMissing),
          note: "traced but absent",
          tone: "warn",
        }
      : null,
  ].filter(Boolean));

  refresh();

  return {
    title: `Universes · ${campaignId} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      stats,
      panel({
        title: "Universes",
        hint: "Ordered by universe index. The table is paginated; filters stay in the URL.",
        body: el("div", {}, form, summaryHost, resultsHost),
        flush: true,
      }),
    ),
  };
}
