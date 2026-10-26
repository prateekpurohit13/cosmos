import { api } from "../lib/api.js";
import {
  badge,
  buildContext,
  classChip,
  codeBlock,
  copyButton,
  detailsBlock,
  emptyState,
  kvList,
  notice,
  pageHeader,
  panel,
  reproIdentity,
  reproInvocation,
  statGrid,
  statusBadge,
} from "../lib/components.js";
import { button, el, link } from "../lib/dom.js";
import { formatAt, formatCount, formatNs, shortId } from "../lib/format.js";
import { createTimeline } from "../lib/timeline.js";
import {
  entryClass,
  entryMatches,
  entryOutcome,
  entrySite,
  entryStatus,
  findLedgerMatch,
  laneOrder,
  parseSearch,
  selectedSeqFromQuery,
  traceCounts,
  traceEntryTone,
  traceSearchHelp,
} from "../lib/trace.js";
import { setBuildContext } from "../lib/shell.js";

const PAGE_SIZE = 100;

function pageBounds(page, total) {
  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));
  const current = Math.min(Math.max(1, page), totalPages);
  const start = (current - 1) * PAGE_SIZE;
  return {
    page: current,
    totalPages,
    start,
    end: Math.min(total, start + PAGE_SIZE),
    rows: Array.from({ length: Math.min(PAGE_SIZE, Math.max(0, total - start)) }, (_, offset) => start + offset),
  };
}

function pager(page, bounds, total, matching, onChange) {
  return el(
    "div",
    { class: "pager" },
    el(
      "span",
      {},
      `${formatCount(bounds.start + 1)}–${formatCount(bounds.end)} of ${formatCount(total)} events` +
        (matching !== total ? ` · ${formatCount(matching)} matching` : "") +
        ` · page ${formatCount(bounds.page)} of ${formatCount(bounds.totalPages)}`,
    ),
    el(
      "div",
      { class: "pager__buttons" },
      button("First", () => onChange(1), {
        class: "btn btn--ghost btn--sm",
        disabled: bounds.page <= 1,
      }),
      button("Previous", () => onChange(bounds.page - 1), {
        class: "btn btn--ghost btn--sm",
        disabled: bounds.page <= 1,
      }),
      button("Next", () => onChange(bounds.page + 1), {
        class: "btn btn--ghost btn--sm",
        disabled: bounds.page >= bounds.totalPages,
      }),
      button("Last", () => onChange(bounds.totalPages), {
        class: "btn btn--ghost btn--sm",
        disabled: bounds.page >= bounds.totalPages,
      }),
    ),
  );
}

function traceTable(entries, indexes, bounds, selected, onSelect) {
  if (entries.length === 0) {
    return emptyState({
      title: "No trace entries",
      body: "The canonical trace is present, but it recorded no post-gate fault decisions.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--trace" },
      el("caption", { class: "sr-only" }, "Canonical fault trace entries in event-sequence order"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col", class: "num" }, "#"),
          el("th", { scope: "col", class: "num" }, "Virtual time"),
          el("th", { scope: "col" }, "Class"),
          el("th", { scope: "col" }, "Site"),
          el("th", { scope: "col" }, "Outcome"),
          el("th", { scope: "col" }, "Status"),
          el("th", { scope: "col" }, "Drew"),
          el("th", { scope: "col", class: "num" }, "Eligible"),
          el("th", { scope: "col", class: "num" }, "Amount"),
        ),
      ),
      el(
        "tbody",
        {},
        ...bounds.rows.map((index) => {
          const entry = entries[index];
          const matched = indexes.matches[index];
          const isSelected = selected !== null && Number(selected) === Number(entry?.seq);
          const row = el(
            "tr",
            {
              class: [
                matched ? "" : "is-dim",
                isSelected ? "is-selected" : "",
              ]
                .filter(Boolean)
                .join(" "),
              tabindex: "0",
              title: matched ? "Open this event in the inspector" : "This event is dimmed by the search",
              onClick: () => onSelect(entry, index),
              onKeydown: (event) => {
                if (event.key === "Enter" || event.key === " ") {
                  event.preventDefault();
                  onSelect(entry, index);
                }
              },
            },
            el("td", { class: "num" }, formatCount(entry?.seq)),
            el("td", { class: "num" }, formatAt(entry?.at_ns)),
            el("td", {}, classChip(entryClass(entry))),
            el("td", {}, el("span", { class: "mono" }, entrySite(entry))),
            el("td", {}, el("span", { class: "mono" }, entryOutcome(entry))),
            el(
              "td",
              {},
              el(
                "span",
                { class: `status status--${traceEntryTone(entry)}` },
                entryStatus(entry),
              ),
            ),
            el("td", {}, entry?.drew ? "yes" : "no"),
            el("td", { class: "num" }, formatCount(entry?.eligible_index)),
            el("td", { class: "num" }, formatNs(entry?.amount_ns)),
          );
          return row;
        }),
      ),
    ),
  );
}

function laneLegend(entries) {
  const counts = traceCounts(entries);
  const lanes = laneOrder(counts);
  return el(
    "div",
    { class: "timeline-legend", "aria-label": "Trace classes" },
    ...lanes.map((lane) =>
      el(
        "span",
        { class: "timeline-legend__item" },
        classChip(lane),
        el("span", { class: "faint" }, formatCount(counts.classes.get(lane) ?? 0)),
      ),
    ),
  );
}

function inspectorBody(entry, index, entries, universe, onSelect, onPrevious, onNext) {
  if (!entry) {
    return emptyState({
      title: "Select an event",
      body: "Click a marker on the timeline, use the arrow keys, or select a row in the trace table to inspect the frozen decision fields.",
    });
  }
  const ledger = findLedgerMatch(universe?.ledger, entry);
  const fields = [
    ["Event sequence", el("span", { class: "mono" }, formatCount(entry.seq))],
    ["Virtual time", el("span", { class: "mono" }, formatAt(entry.at_ns))],
    ["Site", el("span", { class: "mono" }, entrySite(entry))],
    ["Class", classChip(entryClass(entry))],
    ["Outcome", el("span", { class: "mono" }, entryOutcome(entry))],
    [
      "Status",
      el(
        "span",
        { class: `status status--${traceEntryTone(entry)}` },
        entryStatus(entry),
      ),
    ],
    ["Drew", entry?.drew ? "yes" : "no"],
    ["Eligible call", el("span", { class: "mono" }, `#${formatCount(entry.eligible_index)}`)],
    ["Amount", formatNs(entry.amount_ns)],
    ["Source", el("span", { class: "mono" }, `canonical trace · site id ${String(entry.site_id)}`)],
  ];
  return el(
    "div",
    { class: "timeline-inspector__body" },
    el(
      "div",
      { class: "timeline-inspector__head" },
      el("h3", { class: "timeline-inspector__title" }, `Event #${formatCount(entry.seq)}`),
      el(
        "div",
        { class: "cluster" },
        el(
          "span",
          { class: `status status--${traceEntryTone(entry)}` },
          entryStatus(entry),
        ),
        classChip(entryClass(entry)),
      ),
    ),
    kvList(fields, { className: "kv--nowrap" }),
    ledger
      ? detailsBlock(
          "Matching ledger row",
          el(
            "div",
            {},
            kvList([
              ["Fire index", formatCount(ledger.fire_index)],
              ["Ledger status", ledger.status ?? "—"],
              ["Reason", ledger.reason ?? "none"],
              ["Persistent", ledger.persistent ? "yes" : "no"],
            ]),
          ),
          true,
        )
      : notice(
          "info",
          "No exact ledger row",
          "The canonical trace records no reason field. A ledger row is shown only when site, eligible call and virtual time identify the same decision.",
        ),
    notice(
      "info",
      "Correlation metadata absent",
      "Node, task and correlation ids are not part of the canonical fault trace. They belong to the separate v3 UI event stream (D5).",
    ),
    detailsBlock("Raw trace entry", codeBlock(JSON.stringify(entry, null, 2))),
    el(
      "div",
      { class: "cluster section-gap" },
      button("Previous event", onPrevious, {
        class: "btn btn--ghost btn--sm",
        disabled: index <= 0,
      }),
      button("Next event", onNext, {
        class: "btn btn--ghost btn--sm",
        disabled: index < 0 || index >= entries.length - 1,
      }),
      copyButton(
        () => {
          const params = new URLSearchParams(window.location.search);
          params.set("event", String(entry.seq));
          return `${window.location.origin}${window.location.pathname}?${params.toString()}`;
        },
        "Copy event link",
      ),
    ),
  );
}

export async function timelineView({ campaignId, seed, signal }) {
  const params = new URLSearchParams(window.location.search);
  const initialQuery = params.get("q") ?? "";
  const initialSeq = selectedSeqFromQuery(params);

  const [universeResult, traceResult] = await Promise.allSettled([
    api.universe(campaignId, seed, signal),
    api.trace(campaignId, seed, signal),
  ]);
  if (universeResult.status === "rejected") throw universeResult.reason;
  if (traceResult.status === "rejected" && traceResult.reason?.name === "AbortError") {
    throw traceResult.reason;
  }

  const universeData = universeResult.value;
  const metadata = traceResult.status === "fulfilled" ? traceResult.value : null;
  const traceError = traceResult.status === "rejected" ? traceResult.reason : null;
  const manifest = universeData.manifest ?? metadata?.manifest ?? {};
  const universe = universeData.universe ?? metadata?.universe ?? {};
  const campaign = universeData.campaign ?? {};
  const trace = metadata?.trace ?? {
    present: false,
    traced: universe?.trace?.traced ?? false,
    missing: false,
    entries: [],
  };
  const entries = (Array.isArray(trace.entries) ? trace.entries : []).filter(
    (entry) => entry && typeof entry === "object",
  );
  const counts = traceCounts(entries);
  const lanes = laneOrder(counts);
  const status = universe.status ?? "unknown";
  const seedValue = universe.seed ?? seed;
  const invocation = reproInvocation(manifest, seedValue);
  const identity = reproIdentity(manifest, seedValue);
  const findingsSeed = (Array.isArray(campaign.findings) ? campaign.findings : []).find(
    (finding) => {
      const seeds = Array.isArray(finding?.seeds) ? finding.seeds : [];
      return seeds.some((item) => typeof item === "string" && item === String(universe.seed ?? seed));
    },
  );

  setBuildContext(buildContext(manifest));

  const badges = [
    statusBadge(status),
    findingsSeed ? badge(String(findingsSeed.id), "bad") : null,
    trace.present ? badge("trace", "info") : badge("no trace", "warn"),
    Number(trace.dropped) > 0 ? badge(`${formatCount(trace.dropped)} dropped`, "warn") : null,
    trace.hash_matches === false ? badge("hash mismatch", "bad") : null,
  ].filter(Boolean);

  const notices = [];
  const eventParam = params.get("event");
  if (eventParam !== null) {
    if (initialSeq === null) {
      notices.push(
        notice(
          "warn",
          "Unknown event in link",
          `The event parameter ${JSON.stringify(eventParam)} is not a decimal sequence number; no event was selected.`,
        ),
      );
    } else if (!entries.some((entry) => Number(entry.seq) === initialSeq)) {
      notices.push(
        notice(
          "warn",
          "Unknown event in link",
          `Event #${formatCount(initialSeq)} is not present in this trace; no event was selected.`,
        ),
      );
    }
  }
  if (traceError) {
    notices.push(
      notice(
        "bad",
        "Trace could not be loaded",
        traceError.message ||
          "The canonical trace could not be loaded. The rest of the universe report is still available.",
      ),
    );
  } else if (!trace.present) {
    notices.push(
      notice(
        "warn",
        trace.missing ? "Trace file missing" : "No canonical trace",
        trace.missing
          ? "The artifact metadata says a trace was captured, but traces/... is absent. Do not treat this as a complete replay."
          : trace.traced
            ? "This universe was traced, but no trace bytes were exported."
            : "This campaign did not capture canonical trace bytes for this universe.",
      ),
    );
  }
  if (Number(trace.dropped) > 0) {
    notices.push(
      notice(
        "warn",
        "Trace evidence truncated",
        `The fixed-capacity buffer dropped ${formatCount(trace.dropped)} decisions. The timeline starts at the first recorded entry and cannot show the missing tail.`,
      ),
    );
  }
  if (trace.hash_matches === false) {
    notices.push(
      notice(
        "bad",
        "Trace hash mismatch",
        `The canonical bytes hash to ${trace.computed_hash}, but the artifact declares ${trace.declared_hash}. Treat this trace as unverified evidence.`,
      ),
    );
  }
  if (trace.seed_matches === false) {
    notices.push(
      notice(
        "bad",
        "Trace seed mismatch",
        `The trace header names seed ${trace.seed}, but this universe is seed ${universe.seed ?? seed}. The trace does not belong to this universe identity.`,
      ),
    );
  }
  if (trace.present && entries.length === 0 && !traceError) {
    notices.push(
      notice(
        "info",
        "No post-gate decisions recorded",
        "A trace entry is written only after the injector's gate chain. This universe reached no eligible, post-gate decision.",
      ),
    );
  }

  const spanNote =
    counts.minAt === null
      ? "no virtual time recorded"
      : `${formatAt(counts.minAt)} → ${formatAt(counts.maxAt)}`;

  const stats = statGrid([
    { label: "Entries", value: formatCount(entries.length), note: "canonical decisions", tone: "accent" },
    {
      label: "Injected",
      value: formatCount(counts.injected),
      note: "outcome not None",
      tone: counts.injected ? "warn" : "neutral",
    },
    {
      label: "No fault",
      value: formatCount(counts.noFault),
      note: "outcome None",
      tone: "neutral",
    },
    {
      label: "Suppressed",
      value: formatCount(counts.suppressed),
      note: `skipped ${formatCount(counts.skipped)}`,
      tone: counts.suppressed ? "verify" : "neutral",
    },
    { label: "Classes", value: formatCount(lanes.length), note: lanes.join(" · ") || "none" },
    {
      label: "Virtual span",
      value: counts.minAt === null ? "—" : formatNs(Math.max(0, counts.maxAt - counts.minAt)),
      note: spanNote,
    },
    {
      label: "Trace hash",
      value: shortId(trace.computed_hash ?? trace.declared_hash),
      note:
        trace.hash_matches === true
          ? "matches declared hash"
          : trace.hash_matches === false
            ? "does not match"
            : "no declared hash",
      tone: trace.hash_matches === false ? "bad" : trace.hash_matches === true ? "ok" : "neutral",
    },
    {
      label: "Capacity",
      value: formatCount(trace.capacity),
      note: Number(trace.dropped) > 0 ? `${formatCount(trace.dropped)} dropped` : "no drops recorded",
      tone: Number(trace.dropped) > 0 ? "warn" : "neutral",
    },
  ]);

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId} / universe / timeline`,
    title: "Fault timeline",
    subtitle: el(
      "span",
      {},
      "Universe ",
      el("code", { class: "inline-code" }, `#${formatCount(universe.index)}`),
      " · seed ",
      el("code", { class: "inline-code" }, String(seedValue)),
      universe.seed_hex ? el("span", { class: "faint" }, ` · ${universe.seed_hex}`) : null,
    ),
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
      {
        label: `#${formatCount(universe.index)}`,
        href: `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(seedValue)}`,
      },
      { label: "Timeline" },
    ],
    badges,
    actions: [
      link(
        `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(seedValue)}`,
        "Back to universe",
        { class: "btn btn--ghost" },
      ),
      copyButton(invocation ?? identity, invocation ? "Copy repro" : "Copy repro identity", {
        class: "btn btn--primary",
        title: invocation ?? identity,
      }),
    ],
  });

  let selected = null;
  let tokens = parseSearch(initialQuery);
  let matchFlags = entries.map((entry) => entryMatches(entry, tokens));
  let matchCount = tokens.length === 0 ? entries.length : matchFlags.filter(Boolean).length;
  let page = 1;
  let searchTimer = 0;
  let urlTimer = 0;
  let controller = null;

  const searchInput = el("input", {
    id: "timeline-search",
    name: "q",
    type: "search",
    placeholder: traceSearchHelp(),
    autocomplete: "off",
    spellcheck: "false",
    value: initialQuery,
  });
  const searchSummary = el("span", {
    class: "timeline-search__summary",
    role: "status",
    "aria-live": "polite",
  });
  const previousMatch = button("Previous match", () => gotoMatch(-1), {
    class: "btn btn--ghost btn--sm",
    disabled: true,
  });
  const nextMatch = button("Next match", () => gotoMatch(1), {
    class: "btn btn--ghost btn--sm",
    disabled: true,
  });
  const clearSearch = button(
    "Clear",
    () => {
      searchInput.value = "";
      applySearch();
      searchInput.focus();
    },
    { class: "btn btn--ghost btn--sm" },
  );
  const toolbar = el(
    "form",
    {
      class: "timeline-toolbar",
      role: "search",
      onSubmit: (event) => {
        event.preventDefault();
        applySearch();
      },
    },
    el(
      "div",
      { class: "field field--grow" },
      el("label", { for: "timeline-search" }, "Search events"),
      searchInput,
    ),
    el(
      "div",
      { class: "timeline-toolbar__actions" },
      previousMatch,
      nextMatch,
      clearSearch,
      searchSummary,
    ),
  );

  const timelineHost = el("div", { class: "timeline-host" });
  const inspectorHost = el("div", { class: "timeline-inspector" });
  const tableHost = el("div", {});
  const pagerHost = el("div", {});

  const selectedIndex = () => {
    if (selected === null) return -1;
    return entries.findIndex((entry) => Number(entry.seq) === Number(selected));
  };

  const syncUrl = () => {
    const path = window.location.pathname;
    const next = new URLSearchParams();
    if (searchInput.value) next.set("q", searchInput.value);
    if (selected !== null) next.set("event", String(selected));
    const query = next.toString();
    window.clearTimeout(urlTimer);
    urlTimer = window.setTimeout(() => {
      if (window.location.pathname !== path) return;
      window.history.replaceState(null, "", query ? `${path}?${query}` : path);
    }, 150);
  };

  const renderInspector = (entry, index) => {
    inspectorHost.replaceChildren(
      inspectorBody(
        entry,
        index,
        entries,
        universe,
        (nextEntry, nextIndex) => selectEntry(nextEntry, nextIndex),
        () => selectEntry(entries[index - 1], index - 1),
        () => selectEntry(entries[index + 1], index + 1),
      ),
    );
  };

  const rematch = () => {
    matchFlags = entries.map((entry) => entryMatches(entry, tokens));
    matchCount = tokens.length === 0 ? entries.length : matchFlags.filter(Boolean).length;
  };

  const renderTable = () => {
    const bounds = pageBounds(page, entries.length);
    page = bounds.page;
    tableHost.replaceChildren(
      traceTable(entries, { matches: matchFlags }, bounds, selected, (entry, index) =>
        selectEntry(entry, index),
      ),
    );
    pagerHost.replaceChildren();
    if (entries.length > 0) {
      pagerHost.append(
        pager(page, bounds, entries.length, matchCount, (nextPage) => {
          page = nextPage;
          renderTable();
        }),
      );
    }
    searchSummary.textContent =
      tokens.length === 0
        ? `${formatCount(entries.length)} events`
        : `${formatCount(matchCount)} of ${formatCount(entries.length)} events match`;
    previousMatch.disabled = matchCount === 0;
    nextMatch.disabled = matchCount === 0;
  };

  const selectEntry = (entry, index, { fromCanvas = false } = {}) => {
    if (!entry || index < 0 || index >= entries.length) return;
    selected = Number(entry.seq);
    if (!fromCanvas) controller?.setSelected(selected);
    renderInspector(entry, index);
    const targetPage = Math.floor(index / PAGE_SIZE) + 1;
    if (targetPage !== page) page = targetPage;
    renderTable();
    syncUrl();
  };

  const gotoMatch = (direction) => {
    const indexes = [];
    matchFlags.forEach((matched, index) => {
      if (matched) indexes.push(index);
    });
    if (indexes.length === 0) return;
    const current = selectedIndex();
    let target;
    if (direction > 0) {
      target = indexes.find((index) => index > current);
      if (target === undefined) target = indexes[0];
    } else {
      target = [...indexes].reverse().find((index) => index < current);
      if (target === undefined) target = indexes[indexes.length - 1];
    }
    selectEntry(entries[target], target);
  };

  const applySearch = () => {
    tokens = parseSearch(searchInput.value);
    rematch();
    controller?.setTokens(tokens);
    renderTable();
    syncUrl();
  };

  const onSearchInput = () => {
    window.clearTimeout(searchTimer);
    searchTimer = window.setTimeout(applySearch, 120);
  };
  searchInput.addEventListener("input", onSearchInput);

  if (entries.length > 0) {
    controller = createTimeline({
      entries,
      tokens,
      selectedSeq: initialSeq,
      onSelect: (entry, index) => selectEntry(entry, index, { fromCanvas: true }),
    });
    timelineHost.replaceChildren(
      laneLegend(entries),
      controller.node,
    );
    const initialIndex = initialSeq === null ? -1 : selectedIndexFor(initialSeq, entries);
    if (initialIndex >= 0) {
      selected = initialSeq;
      controller.setSelected(selected);
      page = Math.floor(initialIndex / PAGE_SIZE) + 1;
      renderInspector(entries[initialIndex], initialIndex);
    } else {
      renderInspector(null, -1);
    }
  } else {
    timelineHost.replaceChildren(
      emptyState({
        title: "Nothing to draw",
        body: trace.present
          ? "The trace has no post-gate decisions, so there is no event sequence."
          : "Replay evidence is unavailable for this universe.",
      }),
    );
    renderInspector(null, -1);
  }

  renderTable();

  const timelinePanel = panel({
    title: "Fault timeline",
    subtitle: "Canonical decisions in event-sequence order; search dims non-matching events",
    count: entries.length,
    flush: true,
    body: el("div", { class: "timeline-panel" }, toolbar, timelineHost),
  });

  const inspectorPanel = panel({
    title: "Inspector",
    subtitle: "Frozen TraceEntry fields; no invented node or correlation data",
    body: inspectorHost,
  });

  const traceTablePanel = panel({
    title: "Trace entries",
    subtitle: "Every recorded decision, in event-sequence order. Select a row to inspect it.",
    count: entries.length,
    flush: true,
    body: el("div", {}, tableHost, pagerHost),
  });

  const rawPanel = panel({
    title: "Trace evidence",
    subtitle: "Canonical encoding metadata",
    body: el(
      "div",
      {},
      kvList([
        ["Trace path", trace.path || "—"],
        ["Declared hash", trace.declared_hash || "—"],
        ["Computed hash", trace.computed_hash || "—"],
        ["Hash verified", trace.hash_matches === true ? "yes" : trace.hash_matches === false ? "no" : "not declared"],
        ["Header seed", trace.seed ? `${trace.seed} (${trace.seed_hex || "—"})` : "—"],
        ["Header seed matches", trace.seed_matches === true ? "yes" : trace.seed_matches === false ? "no" : "—"],
        ["Entries", formatCount(trace.count ?? entries.length)],
        ["Bytes", formatCount(trace.size_bytes)],
        ["Capacity", formatCount(trace.capacity)],
        ["Dropped", formatCount(trace.dropped)],
      ]),
      notice(
        "info",
        "Canonical trace untouched",
        "This view decodes the exact bytes trace_hash() covers. The UI does not add UI-only events to the canonical trace; richer scheduler, network and application telemetry belongs to the separate v3 stream.",
      ),
    ),
  });

  const node = el(
    "div",
    { class: "page" },
    header,
    stats,
    notices.length > 0 ? el("div", { class: "stack" }, ...notices) : null,
    el(
      "div",
      { class: "timeline-layout section-gap" },
      timelinePanel,
      inspectorPanel,
    ),
    el("div", { class: "section-gap" }, traceTablePanel),
    el("div", { class: "section-gap" }, rawPanel),
  );

  return {
    title: `Timeline · universe #${formatCount(universe.index)} · ${campaign.name || campaignId} · Cosmos`,
    node,
    destroy() {
      window.clearTimeout(searchTimer);
      window.clearTimeout(urlTimer);
      searchInput.removeEventListener("input", onSearchInput);
      controller?.destroy();
    },
  };
}

function selectedIndexFor(seq, entries) {
  return entries.findIndex((entry) => Number(entry.seq) === Number(seq));
}
