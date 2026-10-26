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
} from "../lib/components.js";
import { button, el, link } from "../lib/dom.js";
import { formatAt, formatCount, formatNs, shortId } from "../lib/format.js";
import {
  DEFAULT_PLAYBACK_SPEED,
  PLAYBACK_SPEEDS,
  failureRelationLabel,
  groupFailuresByIndex,
  isInjected,
  isSuppressedOrSkipped,
  lastIndexAtOrBefore,
  nextIndex,
  parseVirtualTimeNs,
  playbackIntervalMs,
  resolveFailedChecks,
} from "../lib/playback.js";
import { createTimeline } from "../lib/timeline.js";
import {
  entryClass,
  entryMatches,
  entryOutcome,
  entrySite,
  entryStatus,
  findLedgerMatch,
  finiteTimeNs,
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
              dataset: { index: String(index) },
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

function laneLegend(entries, failures = [], markerCount = 0) {
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
    failures.length > 0
      ? el(
          "span",
          { class: "timeline-legend__item" },
          badge(
            `${formatCount(failures.length)} failed check${failures.length === 1 ? "" : "s"}`,
            "bad",
          ),
          el(
            "span",
            { class: "faint" },
            markerCount > 0 ? "dashed red marker" : "no marker position",
          ),
        )
      : null,
  );
}

function inspectorBody(entry, index, entries, ledger, failuresAt, onSelect, onPrevious, onNext) {
  if (!entry) {
    return emptyState({
      title: "Select an event",
      body: "Click a marker on the timeline, use the arrow keys, or select a row in the trace table to inspect the frozen decision fields.",
    });
  }
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
    failuresAt.length > 0
      ? notice(
          "bad",
          failuresAt.length === 1
            ? `Failed check: ${failuresAt[0].id}`
            : `${formatCount(failuresAt.length)} failed checks at this event`,
          failuresAt
            .map(
              (failure) =>
                `${failure.id} at ${formatAt(failure.atNs)} (${failureRelationLabel(failure)})`,
            )
            .join(". ") +
            ". The canonical trace records no check event; this marker is the closest recorded decision, not proof that the event caused the failure.",
        )
      : null,
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
          params.delete("check");
          params.delete("at");
          params.set("event", String(entry.seq));
          return `${window.location.origin}${window.location.pathname}?${params.toString()}`;
        },
        "Copy event link",
      ),
    ),
  );
}

function eventHref(seq) {
  const search = new URLSearchParams(window.location.search);
  search.delete("check");
  search.delete("at");
  search.set("event", String(seq));
  return `${window.location.pathname}?${search.toString()}`;
}

function failedChecksBody(failures, entries, campaignId, findingIds, onJump) {
  if (failures.length === 0) {
    return emptyState({
      title: "No failed checks",
      body: "This universe report has no failed check to mark on the timeline.",
    });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data data--failures" },
      el("caption", { class: "sr-only" }, "Failed checks and their nearest recorded events"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Check"),
          el("th", { scope: "col", class: "num" }, "Virtual time"),
          el("th", { scope: "col", class: "num" }, "Nearest event"),
          el("th", { scope: "col" }, "Relation"),
          el("th", { scope: "col" }, "Detail"),
        ),
      ),
      el(
        "tbody",
        {},
        ...failures.map((failure) => {
          const entry = failure.index >= 0 ? entries[failure.index] : null;
          return el(
            "tr",
            {},
            el(
              "td",
              {},
              findingIds.has(failure.id)
                ? findingLink(campaignId, failure.id)
                : el("span", { class: "mono" }, failure.id),
            ),
            el("td", { class: "num" }, formatAt(failure.atNs)),
            el(
              "td",
              { class: "num" },
              entry
                ? link(eventHref(entry.seq), `#${formatCount(entry.seq)}`, {
                    class: "mono",
                    title: `Open event #${formatCount(entry.seq)} at ${formatAt(entry.at_ns)}`,
                    onClick: (event) => {
                      if (
                        event.button !== 0 ||
                        event.metaKey ||
                        event.ctrlKey ||
                        event.shiftKey ||
                        event.altKey
                      ) {
                        return;
                      }
                      event.preventDefault();
                      onJump(entry, failure.index);
                    },
                  })
                : el("span", { class: "faint" }, "—"),
            ),
            el("td", { class: "text-soft" }, failureRelationLabel(failure)),
            el("td", { class: "text-soft" }, failure.detail || "—"),
          );
        }),
      ),
    ),
  );
}

export async function timelineView({ campaignId, seed, signal }) {
  const params = new URLSearchParams(window.location.search);
  const viewPath = window.location.pathname;
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
  const failures = resolveFailedChecks(universe, entries);
  const failureGroups = groupFailuresByIndex(failures);
  const failureByIndex = new Map(failureGroups.map((group) => [group.index, group]));
  const failureIndexes = new Set(
    failures.map((failure) => failure.index).filter((index) => index >= 0),
  );
  const findingIds = new Set(
    (Array.isArray(campaign.findings) ? campaign.findings : [])
      .filter((finding) => finding && typeof finding === "object")
      .map((finding) => String(finding.id)),
  );
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

  const droppedDeclared = Number.isInteger(trace.dropped);
  const droppedCount = droppedDeclared ? trace.dropped : null;
  const badges = [
    statusBadge(status),
    findingsSeed ? badge(String(findingsSeed.id), "bad") : null,
    trace.present ? badge("trace", "info") : badge("no trace", "warn"),
    droppedCount !== null && droppedCount > 0
      ? badge(`${formatCount(droppedCount)} dropped`, "warn")
      : null,
    trace.present && !droppedDeclared ? badge("drop count missing", "warn") : null,
    trace.hash_matches === false ? badge("hash mismatch", "bad") : null,
  ].filter(Boolean);

  const notices = [];
  const eventParam = params.get("event");
  const checkParam = params.get("check");
  const atParam = params.get("at");
  const atTarget = atParam === null ? null : parseVirtualTimeNs(atParam);
  const checks = Array.isArray(universe.checks) ? universe.checks : [];
  let initialSelection = null;

  const resolveTarget = (targetNs, source) => {
    const resolved = lastIndexAtOrBefore(entries, targetNs);
    if (resolved.index < 0) return null;
    return {
      index: resolved.index,
      seq: Number(entries[resolved.index]?.seq),
      atOrBefore: resolved.atOrBefore,
      targetNs,
      source,
    };
  };

  if (initialSeq !== null) {
    const index = entries.findIndex((entry) => Number(entry.seq) === initialSeq);
    if (index >= 0) {
      initialSelection = { index, seq: initialSeq, atOrBefore: true, source: "event" };
    } else {
      notices.push(
        notice(
          "warn",
          "Unknown event in link",
          `Event #${formatCount(initialSeq)} is not present in this trace; the event parameter was ignored.`,
        ),
      );
    }
  } else if (eventParam !== null) {
    notices.push(
      notice(
        "warn",
        "Unknown event in link",
        `The event parameter ${JSON.stringify(eventParam)} is not a decimal sequence number; it was ignored.`,
      ),
    );
  }

  if (!initialSelection && checkParam) {
    const check = checks.find((item) => String(item?.id) === checkParam);
    const checkAt = check ? finiteTimeNs(check.at_ns) : null;
    if (!check) {
      notices.push(
        notice(
          "warn",
          "Check not found in link",
          `This universe has no check with id ${JSON.stringify(checkParam)}. The check parameter was ignored.`,
        ),
      );
    } else if (checkAt === null) {
      notices.push(
        notice(
          "warn",
          "Check has no virtual time",
          `Check ${JSON.stringify(checkParam)} has no usable at_ns, so the timeline cannot resolve an event for it.`,
        ),
      );
    } else {
      initialSelection = resolveTarget(checkAt, "check");
      if (!initialSelection) {
        notices.push(
          notice(
            "warn",
            "No event for failed check",
            `Check ${JSON.stringify(checkParam)} has no recorded event to jump to.`,
          ),
        );
      }
    }
  }

  if (!initialSelection && atParam !== null) {
    if (atTarget === null) {
      notices.push(
        notice(
          "warn",
          "Invalid virtual time in link",
          `The at parameter ${JSON.stringify(atParam)} is not a virtual time such as 0, 2500000, 2.5ms or 1s.`,
        ),
      );
    } else {
      initialSelection = resolveTarget(atTarget, "at");
      if (!initialSelection) {
        notices.push(
          notice(
            "warn",
            "No event to jump to",
            "This universe has no recorded event for the requested virtual time.",
          ),
        );
      }
    }
  }

  if (initialSelection && initialSelection.source === "check") {
    const failed = failures.find((failure) => failure.id === String(checkParam));
    const checkLabel = failed ? "failed check" : "check";
    if (counts.maxAt !== null && initialSelection.targetNs > counts.maxAt) {
      notices.push(
        notice(
          "info",
          "Target after the last recorded event",
          `The ${checkLabel} is at ${formatAt(initialSelection.targetNs)}, but the trace ends at ` +
            `${formatAt(counts.maxAt)}. Event #${formatCount(initialSelection.seq)} is the last ` +
            `recorded decision, not the target point.`,
        ),
      );
    } else if (!initialSelection.atOrBefore) {
      notices.push(
        notice(
          "warn",
          "Jump target precedes the trace",
          `The ${checkLabel} is at ${formatAt(initialSelection.targetNs)}, before the first recorded ` +
            `decision. Event #${formatCount(initialSelection.seq)} is the closest available evidence.`,
        ),
      );
    }
  } else if (initialSelection && initialSelection.source === "at") {
    if (counts.maxAt !== null && initialSelection.targetNs > counts.maxAt) {
      notices.push(
        notice(
          "info",
          "Target after the last recorded event",
          `The target is ${formatAt(initialSelection.targetNs)}, but the trace ends at ` +
            `${formatAt(counts.maxAt)}. Event #${formatCount(initialSelection.seq)} is the last ` +
            `recorded decision, not the target point.`,
        ),
      );
    } else if (!initialSelection.atOrBefore) {
      notices.push(
        notice(
          "warn",
          "Jump target precedes the trace",
          `No recorded event is at or before ${formatAt(initialSelection.targetNs)}; ` +
            `event #${formatCount(initialSelection.seq)} is the first recorded decision.`,
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
      note: !droppedDeclared
        ? "drop count not declared"
        : droppedCount > 0
          ? `${formatCount(droppedCount)} dropped`
          : "no drops recorded",
      tone: !droppedDeclared ? "warn" : droppedCount > 0 ? "warn" : "neutral",
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
      link(compareHref(campaignId, seedValue), "Compare", {
        class: "btn btn--ghost",
        title: "Compare this universe with another universe in this campaign",
      }),
      link(minimizationHref(campaignId, seedValue), "Minimization", {
        class: "btn btn--ghost",
        title: "Open the exported MinimizeReport, or its explicit absence",
      }),
      copyButton(invocation ?? identity, invocation ? "Copy repro" : "Copy repro identity", {
        class: "btn btn--primary",
        title: invocation ?? identity,
      }),
    ],
  });

  let selected = null;
  let selectedIdx = -1;
  const ledgerCache = new Map();
  let tokens = parseSearch(initialQuery);
  let matchFlags = entries.map((entry) => entryMatches(entry, tokens));
  let matchCount = tokens.length === 0 ? entries.length : matchFlags.filter(Boolean).length;
  let page = 1;
  let searchTimer = 0;
  let urlTimer = 0;
  let playTimer = 0;
  let canvasTimer = 0;
  let pendingCanvasSeq = null;
  let lastCanvasPaint = 0;
  let controller = null;
  let playing = false;
  let speed = DEFAULT_PLAYBACK_SPEED;
  const injectedIndexes = [];
  const suppressedIndexes = [];
  entries.forEach((entry, index) => {
    if (isInjected(entry)) injectedIndexes.push(index);
    if (isSuppressedOrSkipped(entry)) suppressedIndexes.push(index);
  });

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

  const playButton = el(
    "button",
    {
      type: "button",
      class: "btn btn--primary btn--sm",
      "aria-pressed": "false",
      title: "Play or pause the event cursor (Space)",
    },
    "Play",
  );
  playButton.addEventListener("click", () => togglePlayback());
  const stepBackButton = button("Step back", () => stepBy(-1), {
    class: "btn btn--ghost btn--sm",
    title: "Select the previous event; from no selection, the last event (,)",
  });
  const stepButton = button("Step", () => stepBy(1), {
    class: "btn btn--ghost btn--sm",
    title: "Select the next event; from no selection, the first event (.)",
  });
  const nextFaultButton = button(
    "Next injected",
    () => jumpToNext(isInjected, "injected fault"),
    {
      class: "btn btn--ghost btn--sm",
      title: `${formatCount(injectedIndexes.length)} injected decisions; f (wraps)`,
    },
  );
  const nextSkippedButton = button(
    "Next suppressed",
    () => jumpToNext(isSuppressedOrSkipped, "suppressed or skipped decision"),
    {
      class: "btn btn--ghost btn--sm",
      title: `${formatCount(suppressedIndexes.length)} suppressed/skipped decisions; d (wraps)`,
    },
  );
  const nextFailureButton = button(
    "Next failure",
    () => jumpToNext((entry, index) => failureIndexes.has(index), "failed check"),
    {
      class: "btn btn--ghost btn--sm",
      title:
        failureIndexes.size === 0
          ? "No failed check has a mappable recorded event"
          : `${formatCount(failureIndexes.size)} failed check${failureIndexes.size === 1 ? "" : "s"}; jumps to their nearest recorded events (x, wraps)`,
    },
  );
  const speedSelect = el(
    "select",
    {
      id: "timeline-speed",
      class: "select select--sm",
      "aria-label": "Playback speed in events per second",
    },
    ...PLAYBACK_SPEEDS.map((value) => el("option", { value: String(value) }, `${value} ev/s`)),
  );
  speedSelect.value = String(DEFAULT_PLAYBACK_SPEED);
  speedSelect.addEventListener("change", () => {
    const next = Number(speedSelect.value);
    speed = PLAYBACK_SPEEDS.includes(next) ? next : DEFAULT_PLAYBACK_SPEED;
    announce(`Playback speed set to ${speed} events per second.`);
    if (playing) schedulePlayback();
  });
  const timeInput = el("input", {
    id: "timeline-time",
    name: "time",
    type: "text",
    inputmode: "decimal",
    autocomplete: "off",
    spellcheck: "false",
    placeholder: "2.4ms or 2400000ns",
    "aria-label": "Virtual time to jump to",
  });
  const timeGo = button("Jump", () => jumpToTime(), {
    class: "btn btn--ghost btn--sm",
    title: "Select the nearest event at or before this virtual time (g)",
  });
  const timeForm = el(
    "form",
    {
      class: "timeline-time",
      onSubmit: (event) => {
        event.preventDefault();
        jumpToTime();
      },
    },
    el("label", { for: "timeline-time" }, "Jump to virtual time"),
    timeInput,
    timeGo,
  );
  const playbackStatus = el("span", {
    class: "timeline-playback__status",
    role: "status",
    "aria-live": "polite",
  });
  const playbackBar = el(
    "div",
    { class: "timeline-playback" },
    el("div", { class: "timeline-playback__group" }, playButton, stepBackButton, stepButton),
    el(
      "div",
      { class: "timeline-playback__group" },
      nextFaultButton,
      nextSkippedButton,
      nextFailureButton,
    ),
    el(
      "div",
      { class: "timeline-playback__group" },
      el("label", { for: "timeline-speed", class: "timeline-playback__label" }, "Speed"),
      speedSelect,
    ),
    timeForm,
    playbackStatus,
  );

  const timelineHost = el("div", { class: "timeline-host" });
  const inspectorHost = el("div", { class: "timeline-inspector" });
  const tableHost = el("div", {});
  const pagerHost = el("div", {});

  const selectedIndex = () => {
    if (selected === null) return -1;
    if (
      selectedIdx >= 0 &&
      selectedIdx < entries.length &&
      Number(entries[selectedIdx]?.seq) === Number(selected)
    ) {
      return selectedIdx;
    }
    selectedIdx = entries.findIndex((entry) => {
      const seq = Number(entry?.seq);
      return Number.isSafeInteger(seq) && seq >= 0 && seq === Number(selected);
    });
    return selectedIdx;
  };

  const writeUrl = () => {
    const path = window.location.pathname;
    const next = new URLSearchParams();
    if (searchInput.value) next.set("q", searchInput.value);
    if (selected !== null) next.set("event", String(selected));
    const query = next.toString();
    window.history.replaceState(null, "", query ? `${path}?${query}` : path);
  };

  const syncUrl = () => {
    const path = window.location.pathname;
    window.clearTimeout(urlTimer);
    urlTimer = window.setTimeout(() => {
      if (window.location.pathname !== path) return;
      writeUrl();
    }, 150);
  };

  const renderInspector = (entry, index) => {
    const failuresAt = index >= 0 ? failureByIndex.get(index)?.failures ?? [] : [];
    let ledger = null;
    if (entry) {
      const key = JSON.stringify([
        entrySite(entry),
        String(entry?.eligible_index),
        String(entry?.at_ns),
        entryOutcome(entry),
      ]);
      if (ledgerCache.has(key)) {
        ledger = ledgerCache.get(key);
      } else {
        ledger = findLedgerMatch(universe?.ledger, entry);
        ledgerCache.set(key, ledger);
      }
    }
    inspectorHost.replaceChildren(
      inspectorBody(
        entry,
        index,
        entries,
        ledger,
        failuresAt,
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

  const updateTableSelection = () => {
    for (const row of tableHost.querySelectorAll("tr[data-index]")) {
      const index = Number(row.dataset.index);
      const entry = entries[index];
      const isSelected = selected !== null && Number(selected) === Number(entry?.seq);
      row.classList.toggle("is-selected", isSelected);
    }
  };

  const announce = (message) => {
    playbackStatus.textContent = message;
  };

  // A canvas redraw is O(entries) and the minimap rebuilds its buckets on every selection. During
  // playback the cursor can advance faster than a useful paint, so coalesce paints to ~10 Hz; the
  // inspector and table keep up with every event, and pause flushes the latest selection.
  const paintCanvasSelection = (seq) => {
    window.clearTimeout(canvasTimer);
    canvasTimer = 0;
    pendingCanvasSeq = null;
    controller?.setSelected(seq);
    lastCanvasPaint = performance.now();
  };

  const flushCanvasSelection = () => {
    window.clearTimeout(canvasTimer);
    canvasTimer = 0;
    if (pendingCanvasSeq !== null) paintCanvasSelection(pendingCanvasSeq);
    else if (selected !== null) paintCanvasSelection(selected);
  };

  const requestCanvasSelection = (seq) => {
    pendingCanvasSeq = seq;
    const elapsed = performance.now() - lastCanvasPaint;
    if (elapsed >= 100) {
      paintCanvasSelection(seq);
      return;
    }
    if (canvasTimer) return;
    canvasTimer = window.setTimeout(() => {
      canvasTimer = 0;
      if (pendingCanvasSeq !== null) paintCanvasSelection(pendingCanvasSeq);
    }, 100 - elapsed);
  };

  const setPlaying = (next, { flushCanvas = true } = {}) => {
    playing = Boolean(next);
    window.clearTimeout(playTimer);
    playTimer = 0;
    if (!playing) {
      if (flushCanvas) {
        flushCanvasSelection();
      } else {
        window.clearTimeout(canvasTimer);
        canvasTimer = 0;
        pendingCanvasSeq = null;
      }
    }
    playButton.textContent = playing ? "Pause" : "Play";
    playButton.setAttribute("aria-pressed", playing ? "true" : "false");
    syncControls();
  };

  const pausePlayback = ({ flushCanvas = true } = {}) => {
    if (!playing) return false;
    setPlaying(false, { flushCanvas });
    return true;
  };

  const syncControls = () => {
    const current = selectedIndex();
    playButton.textContent = playing ? "Pause" : "Play";
    playButton.disabled = entries.length === 0;
    stepBackButton.disabled = entries.length === 0 || current === 0;
    stepButton.disabled =
      entries.length === 0 || (current >= 0 && current >= entries.length - 1);
    nextFaultButton.disabled = entries.length === 0 || injectedIndexes.length === 0;
    nextSkippedButton.disabled = entries.length === 0 || suppressedIndexes.length === 0;
    nextFailureButton.disabled = entries.length === 0 || failureIndexes.size === 0;
    speedSelect.disabled = entries.length === 0;
    timeInput.disabled = entries.length === 0;
    timeGo.disabled = entries.length === 0;
  };

  const selectEntry = (entry, index, { fromCanvas = false, fromPlayback = false } = {}) => {
    if (!entry || index < 0 || index >= entries.length) return;
    const seq = Number(entry.seq);
    if (!Number.isSafeInteger(seq) || seq < 0) return;
    const paused = !fromPlayback && pausePlayback({ flushCanvas: !fromCanvas });
    selected = seq;
    selectedIdx = index;
    if (fromCanvas) {
      window.clearTimeout(canvasTimer);
      canvasTimer = 0;
      pendingCanvasSeq = null;
      lastCanvasPaint = performance.now();
    }
    if (!fromCanvas) {
      if (fromPlayback) requestCanvasSelection(selected);
      else paintCanvasSelection(selected);
    }
    renderInspector(entry, index);
    const targetPage = Math.floor(index / PAGE_SIZE) + 1;
    if (targetPage !== page) {
      page = targetPage;
      renderTable();
    } else {
      updateTableSelection();
    }
    syncControls();
    syncUrl();
    if (!fromPlayback) {
      announce(paused ? `Paused at event #${formatCount(selected)}.` : `Event #${formatCount(selected)} selected.`);
    }
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
    const paused = pausePlayback();
    tokens = parseSearch(searchInput.value);
    rematch();
    controller?.setTokens(tokens);
    renderTable();
    syncUrl();
    if (paused) announce(`Paused at event #${formatCount(selected)}.`);
  };

  const onSearchInput = () => {
    window.clearTimeout(searchTimer);
    searchTimer = window.setTimeout(applySearch, 120);
  };
  searchInput.addEventListener("input", onSearchInput);

  const schedulePlayback = () => {
    window.clearTimeout(playTimer);
    if (!playing) return;
    playTimer = window.setTimeout(() => {
      playTimer = 0;
      if (!playing) return;
      const current = selectedIndex();
      if (current < 0) {
        selectEntry(entries[0], 0, { fromPlayback: true });
      } else if (current >= entries.length - 1) {
        setPlaying(false);
        announce(`Reached the last event (#${formatCount(selected)}); playback paused.`);
        return;
      } else {
        selectEntry(entries[current + 1], current + 1, { fromPlayback: true });
      }
      schedulePlayback();
    }, playbackIntervalMs(speed));
  };

  const startPlayback = () => {
    if (entries.length === 0) {
      announce("No recorded events to play.");
      return;
    }
    let index = selectedIndex();
    if (index < 0) {
      index = 0;
      selectEntry(entries[0], 0, { fromPlayback: true });
      paintCanvasSelection(selected);
    } else if (index >= entries.length - 1) {
      announce(
        entries.length === 1
          ? "Only one event is recorded; playback cannot advance."
          : `Already at the last event (#${formatCount(selected)}); press Step back or select an earlier event to replay.`,
      );
      return;
    }
    setPlaying(true);
    announce(
      `Playing from event #${formatCount(entries[index].seq)} at ${speed} events per second.`,
    );
    schedulePlayback();
  };

  const togglePlayback = () => {
    if (entries.length === 0) return;
    if (playing) {
      pausePlayback();
      announce(`Paused at event #${formatCount(selected)}.`);
      return;
    }
    startPlayback();
  };

  const stepBy = (delta) => {
    if (entries.length === 0) return;
    pausePlayback();
    const current = selectedIndex();
    const target =
      current < 0
        ? delta > 0
          ? 0
          : entries.length - 1
        : Math.min(entries.length - 1, Math.max(0, current + delta));
    if (current === target) {
      announce(delta > 0 ? "Already at the last event." : "Already at the first event.");
      return;
    }
    selectEntry(entries[target], target);
    announce(`Event #${formatCount(selected)} of ${formatCount(entries.length)}.`);
  };

  const jumpToNext = (predicate, label) => {
    if (entries.length === 0) return;
    const current = selectedIndex();
    const index = nextIndex(entries, current, predicate, { wrap: true });
    if (index < 0) {
      announce(`No ${label} in this trace.`);
      return;
    }
    pausePlayback();
    const wrapped = current >= 0 && index <= current;
    selectEntry(entries[index], index);
    announce(
      index === current
        ? `This is the only ${label}; event #${formatCount(selected)} is already selected.`
        : `${label.charAt(0).toUpperCase()}${label.slice(1)} at event #${formatCount(selected)}` +
            `${wrapped ? " (wrapped to the start)" : ""}.`,
    );
  };

  const jumpToTime = () => {
    if (entries.length === 0) return;
    const targetNs = parseVirtualTimeNs(timeInput.value);
    if (targetNs === null) {
      announce("Enter a virtual time such as 0, 2500000ns, 500µs, 2.4ms or 1s.");
      timeInput.focus();
      return;
    }
    const resolved = lastIndexAtOrBefore(entries, targetNs);
    if (resolved.index < 0) {
      announce("No recorded event to jump to.");
      return;
    }
    pausePlayback();
    selectEntry(entries[resolved.index], resolved.index);
    const afterLast = counts.maxAt !== null && targetNs > counts.maxAt;
    announce(
      `Event #${formatCount(selected)} at ${formatAt(entries[resolved.index].at_ns)} · ` +
        `${
          resolved.atOrBefore
            ? afterLast
              ? "last event (target is after the last recorded event)"
              : "nearest at or before"
            : "first event (no recorded event at or before)"
        } ` +
        `${formatAt(targetNs)}.`,
    );
  };

  const isEditableTarget = (target) => {
    const element = target instanceof Element ? target : null;
    return Boolean(
      element?.closest?.("input, textarea, select, [contenteditable]:not([contenteditable='false'])"),
    );
  };

  // Space activates buttons and summaries natively; every other shortcut may still run there.
  const isNativeSpaceTarget = (target) => {
    const element = target instanceof Element ? target : null;
    return Boolean(
      element?.closest?.("button, summary, [role='button'], [role='checkbox'], [role='switch']"),
    );
  };

  const onKeyDown = (event) => {
    if (
      event.defaultPrevented ||
      event.repeat ||
      event.metaKey ||
      event.ctrlKey ||
      event.altKey
    ) {
      return;
    }
    if (event.key === "Escape" && playing) {
      event.preventDefault();
      pausePlayback();
      announce(`Paused at event #${formatCount(selected)}.`);
      return;
    }
    if (isEditableTarget(event.target)) return;
    if (event.key === " " && isNativeSpaceTarget(event.target)) return;
    if (event.key === " ") {
      event.preventDefault();
      togglePlayback();
      return;
    }
    if (event.key === ".") {
      event.preventDefault();
      stepBy(1);
      return;
    }
    if (event.key === ",") {
      event.preventDefault();
      stepBy(-1);
      return;
    }
    if (event.key === "/") {
      event.preventDefault();
      searchInput.focus();
      searchInput.select();
      return;
    }
    const key = event.key.toLowerCase();
    if (key === "f") {
      event.preventDefault();
      jumpToNext(isInjected, "injected fault");
    } else if (key === "d") {
      event.preventDefault();
      jumpToNext(isSuppressedOrSkipped, "suppressed or skipped decision");
    } else if (key === "x") {
      event.preventDefault();
      jumpToNext((entry, index) => failureIndexes.has(index), "failed check");
    } else if (key === "g") {
      event.preventDefault();
      timeInput.focus();
      timeInput.select();
    }
  };
  window.addEventListener("keydown", onKeyDown);

  if (entries.length > 0) {
    controller = createTimeline({
      entries,
      tokens,
      selectedSeq: initialSelection ? initialSelection.seq : null,
      markers: failureGroups,
      onSelect: (entry, index) => selectEntry(entry, index, { fromCanvas: true }),
    });
    timelineHost.replaceChildren(laneLegend(entries, failures, failureGroups.length), controller.node);
    if (initialSelection) {
      selected = initialSelection.seq;
      selectedIdx = initialSelection.index;
      controller.setSelected(selected);
      page = Math.floor(initialSelection.index / PAGE_SIZE) + 1;
      renderInspector(entries[initialSelection.index], initialSelection.index);
      announce(
        initialSelection.source === "event"
          ? `Event #${formatCount(selected)} selected from the link.`
          : `Event #${formatCount(selected)} selected from the ${initialSelection.source} jump target.`,
      );
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
  syncControls();
  if (!entries.length) {
    announce("No recorded events to play or jump between.");
  }
  if (
    window.location.pathname === viewPath &&
    (initialSelection || eventParam !== null || checkParam !== null || atParam !== null)
  ) {
    writeUrl();
  }

  const timelinePanel = panel({
    title: "Fault timeline",
    subtitle: "Canonical decisions in event-sequence order; search dims non-matching events",
    count: entries.length,
    flush: true,
    body: el("div", { class: "timeline-panel" }, toolbar, playbackBar, timelineHost),
  });

  const inspectorPanel = panel({
    title: "Inspector",
    subtitle: "Frozen TraceEntry fields; no invented node or correlation data",
    body: inspectorHost,
  });

  const failuresPanel =
    failures.length > 0
      ? panel({
          title: "Failed checks",
          subtitle:
            "Nearest recorded event at or before each failure time; the canonical trace has no check event",
          count: failures.length,
          flush: true,
          body: failedChecksBody(failures, entries, campaignId, findingIds, (entry, index) =>
            selectEntry(entry, index),
          ),
        })
      : null;

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
    failuresPanel ? el("div", { class: "section-gap" }, failuresPanel) : null,
    el("div", { class: "section-gap" }, traceTablePanel),
    el("div", { class: "section-gap" }, rawPanel),
  );

  return {
    title: `Timeline · universe #${formatCount(universe.index)} · ${campaign.name || campaignId} · Cosmos`,
    node,
    destroy() {
      window.clearTimeout(searchTimer);
      window.clearTimeout(urlTimer);
      window.clearTimeout(playTimer);
      window.clearTimeout(canvasTimer);
      searchInput.removeEventListener("input", onSearchInput);
      window.removeEventListener("keydown", onKeyDown);
      controller?.destroy();
    },
  };
}
