import { api } from "../lib/api.js";
import {
  badge,
  buildBadges,
  buildContext,
  compareHref,
  copyButton,
  detailsBlock,
  emptyState,
  kvList,
  notice,
  pageHeader,
  panel,
  reproIdentity,
  reproInvocation,
  statusBadge,
  timelineHref,
} from "../lib/components.js";
import { button, el, link } from "../lib/dom.js";
import {
  formatAt,
  formatCount,
  formatNs,
  formatWallNs,
  shortId,
  statusLabel,
} from "../lib/format.js";
import {
  canonicalSeed,
  compareConfigs,
  compareReports,
  compareTraceEntries,
  compareTraceSummaries,
  seedsMatch,
  TRACE_COMPARE_FIELDS,
  traceEntryField,
} from "../lib/compare.js";
import { finiteTimeNs, traceStatusTone } from "../lib/trace.js";
import { navigate } from "../lib/router.js";
import { setBuildContext } from "../lib/shell.js";

const MAX_RENDERED_DIFFS = 200;

function campaignHref(campaignId) {
  return `/campaigns/${encodeURIComponent(String(campaignId))}`;
}

function universeHref(campaignId, seed) {
  return `${campaignHref(campaignId)}/universes/${encodeURIComponent(String(seed))}`;
}

function absentNode() {
  return el("span", { class: "compare-value compare-value--missing" }, "absent");
}

function previewJson(value) {
  if (value === undefined) return { text: "absent", truncated: false };
  if (value === null) return { text: "null", truncated: false };
  if (Array.isArray(value)) {
    return { text: `array[${formatCount(value.length)}]`, truncated: false };
  }
  if (typeof value === "object") {
    return {
      text: `object{${formatCount(Object.keys(value).length)} keys}`,
      truncated: false,
    };
  }
  if (typeof value === "string") {
    const full = JSON.stringify(value);
    if (full.length > 2000) {
      return {
        text: `${full.slice(0, 2000)}… (truncated for display)`,
        truncated: true,
      };
    }
    return { text: full, truncated: false };
  }
  return { text: String(value), truncated: false };
}

function previewJsonNode(value) {
  if (value === undefined) return absentNode();
  if (value === null) {
    return el("span", { class: "compare-value compare-value--null" }, "null");
  }
  const preview = previewJson(value);
  return el(
    "code",
    {
      class: "compare-value",
      ...(preview.truncated ? { title: "Value truncated for display" } : {}),
    },
    preview.text,
  );
}

function rowValueNode(row, value) {
  if (value === undefined) return absentNode();
  if (value === null && row.format === "tri") {
    return el("span", { class: "status status--neutral" }, "not declared");
  }
  if (value === null) {
    return el("span", { class: "compare-value compare-value--null" }, "—");
  }
  if (value === "" && row.format !== "hash" && row.format !== "seed") {
    return el("span", { class: "compare-value compare-value--null" }, "—");
  }
  switch (row.format) {
    case "bool":
      return el(
        "span",
        { class: `status status--${value ? "ok" : "neutral"}` },
        value ? "yes" : "no",
      );
    case "tri":
      return el(
        "span",
        {
          class: `status status--${
            value === true ? "ok" : value === false ? "bad" : "neutral"
          }`,
        },
        value === true ? "yes" : value === false ? "no" : "not declared",
      );
    case "count":
      return el("span", { class: "mono" }, formatCount(value));
    case "time":
      return el("span", { class: "mono" }, formatAt(value));
    case "duration":
      return el("span", { class: "mono" }, formatNs(value));
    case "hash":
      return el("code", { class: "mono", title: String(value) }, shortId(value));
    case "seed":
      return el("code", { class: "mono" }, String(value));
    case "status":
      return statusBadge(String(value));
    default:
      if (typeof value === "object") return previewJsonNode(value);
      return el("span", { class: "compare-value" }, String(value));
  }
}

function rowValueText(row, value) {
  if (value === undefined) return "absent";
  if (value === null && row.format === "tri") return "not declared";
  if (value === null) return "—";
  if (value === "" && row.format !== "hash" && row.format !== "seed") return "—";
  switch (row.format) {
    case "bool":
      return value ? "yes" : "no";
    case "tri":
      return value === true ? "yes" : value === false ? "no" : "not declared";
    case "count":
      return formatCount(value);
    case "time":
      return formatAt(value);
    case "duration":
      return formatNs(value);
    case "hash":
      return shortId(value);
    case "seed":
      return String(value);
    case "status":
      return statusLabel(String(value));
    default:
      return typeof value === "object" ? previewJson(value).text : String(value);
  }
}

function boolText(value) {
  if (value === true) return "yes";
  if (value === false) return "no";
  return "—";
}

function triText(value) {
  if (value === true) return "yes";
  if (value === false) return "no";
  return "not declared";
}

function traceStateText(traceSide) {
  if (!traceSide) return "—";
  if (traceSide.error) return "load error";
  const trace = traceSide?.trace;
  if (!trace) return "—";
  if (trace.present) {
    return trace.count === undefined ? "present" : `${formatCount(trace.count)} entries`;
  }
  if (trace.missing) return "missing file";
  if (trace.traced) return "not exported";
  return "not traced";
}

function findingForSeed(findings, seed) {
  return (
    findings.find(
      (finding) =>
        Array.isArray(finding.seeds) &&
        finding.seeds.some(
          (item) => typeof item === "string" && seedsMatch(item, seed),
        ),
    ) ?? null
  );
}

function firstFailedCheck(universe) {
  const checks = Array.isArray(universe.checks)
    ? universe.checks.filter((check) => check && typeof check === "object")
    : [];
  const failed = checks.filter((check) => check.passed === false);
  return failed.find((check) => finiteTimeNs(check.at_ns) !== null) ?? failed[0] ?? null;
}

function sidePanel({ label, campaignId, universeData, traceSide, campaign }) {
  const universe = universeData.universe ?? {};
  const seed = universe.seed;
  const trace = traceSide?.trace ?? null;
  const checks = Array.isArray(universe.checks)
    ? universe.checks.filter((check) => check && typeof check === "object")
    : [];
  const failedChecks = checks.filter((check) => check.passed === false);
  const siteRows = Object.values(
    universe.sites && typeof universe.sites === "object" && !Array.isArray(universe.sites)
      ? universe.sites
      : {},
  );
  const eligible = siteRows.reduce((total, row) => total + Number(row?.eligible ?? 0), 0);
  const fired = siteRows.reduce((total, row) => total + Number(row?.fired ?? 0), 0);
  const finding = findingForSeed(
    Array.isArray(campaign.findings) ? campaign.findings : [],
    seed,
  );
  const invocation = reproInvocation(universeData.manifest ?? {}, seed);
  const identity = reproIdentity(universeData.manifest ?? {}, seed);
  const failedCheck = firstFailedCheck(universe);
  const timelineTarget = timelineHref(
    campaignId,
    seed,
    failedCheck ? { checkId: failedCheck.id, atNs: failedCheck.at_ns } : {},
  );
  const verifyEnabled = Boolean(universeData.manifest?.config?.verify);
  const verifyFailed =
    universe.verify_failed === true || universe.status === "verify_failed";
  const verifyMismatch =
    typeof universe.verify_mismatch === "string" ? universe.verify_mismatch : "";
  const verifyNotRun = /tracing was not enabled/i.test(verifyMismatch);
  const verifyOverflow = /trace overflowed/i.test(verifyMismatch);
  const verifyLabel = verifyFailed
    ? verifyNotRun
      ? "not run (no trace)"
      : verifyOverflow
        ? "trace overflow"
        : "mismatch"
    : verifyEnabled
      ? "ok"
      : "off";
  const verifyBadge = verifyFailed
    ? verifyNotRun
      ? "verify not run"
      : verifyOverflow
        ? "trace overflow"
        : "verify failed"
    : null;
  const traceHash = trace?.computed_hash ?? trace?.declared_hash ?? "";
  const badges = [
    finding ? badge(String(finding.id), "bad") : null,
    universe.vacuous ? badge("vacuous", "warn") : null,
    verifyBadge ? badge(verifyBadge, "verify") : null,
    universe.died ? badge(`signal ${formatCount(universe.signal)}`, "bad") : null,
    universe.threw ? badge("threw", "bad") : null,
  ].filter(Boolean);

  return panel({
    title: `${label} · universe #${formatCount(universe.index)}`,
    subtitle: `seed ${seed}`,
    className: "compare-side",
    body: el(
      "div",
      {},
      el("div", { class: "cluster" }, statusBadge(universe.status ?? "unknown"), ...badges),
      kvList([
        ["Seed hex", universe.seed_hex || "—"],
        ["Virtual end", formatNs(universe.virtual_end_ns)],
        [
          "Wall duration",
          el(
            "span",
            {},
            formatWallNs(universe.wall_ns),
            el("span", { class: "faint" }, " · host time; not compared"),
          ),
        ],
        [
          "Checks",
          `${formatCount(failedChecks.length)} / ${formatCount(checks.length)} failed`,
        ],
        [
          "Faults",
          `${formatCount(fired)} / ${formatCount(eligible)} fired / eligible`,
        ],
        ["Active allocations", formatCount(universe.active_allocations)],
        [
          "Config hash",
          universe.config_hash
            ? el(
                "code",
                { class: "mono", title: String(universe.config_hash) },
                shortId(universe.config_hash),
              )
            : "—",
        ],
        [
          "Trace",
          trace?.present ? (
            el(
              "span",
              { class: "cluster" },
              el("span", {}, `${formatCount(trace.count)} entries`),
              traceHash
                ? el(
                    "code",
                    { class: "mono", title: String(traceHash) },
                    shortId(traceHash),
                  )
                : null,
              Number(trace.dropped) > 0
                ? badge(`${formatCount(trace.dropped)} dropped`, "warn")
                : null,
            )
          ) : (
            el(
              "span",
              {
                class: `status status--${
                  traceSide?.error ? "bad" : trace?.missing ? "warn" : "neutral"
                }`,
              },
              traceStateText(traceSide),
            )
          ),
        ],
        ["Verify", verifyLabel],
        verifyFailed && universe.verify_mismatch
          ? ["Verify detail", universe.verify_mismatch]
          : null,
      ].filter(Boolean)),
      el(
        "div",
        { class: "cluster section-gap" },
        link(universeHref(campaignId, seed), "Open universe", {
          class: "btn btn--ghost btn--sm",
        }),
        link(timelineTarget, failedCheck ? "Open timeline at failure" : "Open timeline", {
          class: "btn btn--ghost btn--sm",
        }),
        copyButton(invocation ?? identity, invocation ? "Copy repro" : "Copy repro identity", {
          class: "btn btn--ghost btn--sm",
          title: invocation ?? identity,
        }),
      ),
    ),
  });
}

function traceMetaSide(label, traceSide) {
  const body = el("div", { class: "compare-meta" });
  body.append(el("h3", { class: "compare-section-title" }, label));
  if (!traceSide) {
    body.append(emptyState({ title: "No trace metadata", body: "No trace response." }));
    return body;
  }
  if (traceSide.error) {
    body.append(
      notice(
        "bad",
        "Trace could not be loaded",
        traceSide.error.message ||
          "The canonical trace request failed. Report and config comparison are still available.",
      ),
    );
    return body;
  }
  const trace = traceSide.trace;
  if (!trace) {
    body.append(
      emptyState({
        title: "No trace metadata",
        body: "The trace endpoint returned no trace object.",
      }),
    );
    return body;
  }
  body.append(
    kvList([
      ["Present", boolText(trace.present)],
      ["Captured", boolText(trace.traced)],
      ["File missing", boolText(trace.missing)],
      ["Entries", formatCount(trace.count)],
      ["Capacity", formatCount(trace.capacity)],
      ["Dropped", formatCount(trace.dropped)],
      ["Declared hash", trace.declared_hash || "—"],
      ["Computed hash", trace.computed_hash || "—"],
      ["Hash verified", triText(trace.hash_matches)],
      ["Header seed", trace.seed || "—"],
      ["Header seed matches", triText(trace.seed_matches)],
    ]),
  );
  return body;
}

function traceSummaryTable(rows) {
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data compare-table data--compare-summary" },
      el("caption", { class: "sr-only" }, "Side-by-side canonical trace summary"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Trace field"),
          el("th", { scope: "col" }, "Left"),
          el("th", { scope: "col" }, "Right"),
        ),
      ),
      el(
        "tbody",
        {},
        ...rows.map((row) =>
          el(
            "tr",
            { dataset: { different: String(row.different) } },
            el("th", { scope: "row" }, row.label),
            el("td", {}, rowValueNode(row, row.left)),
            el("td", {}, rowValueNode(row, row.right)),
          ),
        ),
      ),
    ),
  );
}

function entryFieldNode(field, value, entry, seed, campaignId) {
  if (!entry) return absentNode();
  switch (field) {
    case "seq": {
      const seq = Number(value);
      if (Number.isSafeInteger(seq) && seq >= 0) {
        return link(timelineHref(campaignId, seed, { eventSeq: seq }), `#${formatCount(seq)}`, {
          class: "mono",
          title: `Open event #${formatCount(seq)} on the timeline`,
        });
      }
      return el("span", { class: "mono" }, value == null ? "—" : String(value));
    }
    case "eligible_index":
      return el("span", { class: "mono" }, formatCount(value));
    case "at_ns":
      return el("span", { class: "mono" }, formatAt(value));
    case "amount_ns":
      return el("span", { class: "mono" }, formatNs(value));
    case "drew":
      return el("span", { class: "status status--neutral" }, value ? "yes" : "no");
    case "status":
      return el(
        "span",
        { class: `status status--${traceStatusTone(value)}` },
        value == null ? "—" : String(value),
      );
    default:
      return el("span", { class: "mono" }, value == null ? "—" : String(value));
  }
}

function eventDivergenceBody({
  comparison,
  leftEntries,
  rightEntries,
  leftSeed,
  rightSeed,
  campaignId,
}) {
  if (!comparison) {
    return notice(
      "info",
      "Trace comparison unavailable",
      "Both universes need a readable canonical trace before entries can be compared.",
    );
  }
  if (comparison.equal) {
    if (comparison.alignedCount === 0) {
      return emptyState({
        title: "No recorded decisions",
        body:
          "Neither trace recorded a post-gate fault decision, so there is nothing to compare. " +
          "This is not evidence that the universes behaved identically.",
      });
    }
    return emptyState({
      title: "No event divergence",
      body:
        `All ${formatCount(comparison.alignedCount)} aligned canonical decisions match. ` +
        "A trace hash difference, when the seeds differ, comes from the canonical header carrying the seed.",
    });
  }

  const first = comparison.first;
  const leftEntry =
    first.index < leftEntries.length ? leftEntries[first.index] : null;
  const rightEntry =
    first.index < rightEntries.length ? rightEntries[first.index] : null;
  const headline =
    first.kind === "length"
      ? first.index === 0
        ? `The ${first.missingSide} trace records no entries; the other side starts at event #${formatCount(
            first.missingSide === "left" ? first.rightSeq : first.leftSeq,
          )}.`
        : `The ${first.missingSide} trace has ${formatCount(first.index)} recorded ` +
          `${first.index === 1 ? "entry" : "entries"}; ` +
          `the other side continues at event #${formatCount(
            first.missingSide === "left" ? first.rightSeq : first.leftSeq,
          )}.`
      : `Position ${formatCount(first.index + 1)} differs first in ${first.fieldLabel}.`;
  const rows = TRACE_COMPARE_FIELDS.map(([field, label]) => {
    const leftValue = leftEntry ? traceEntryField(leftEntry, field) : null;
    const rightValue = rightEntry ? traceEntryField(rightEntry, field) : null;
    const different =
      first.kind === "length" ? !leftEntry || !rightEntry : field === first.field;
    return el(
      "tr",
      { dataset: { different: String(different) } },
      el("th", { scope: "row" }, label),
      el("td", {}, entryFieldNode(field, leftValue, leftEntry, leftSeed, campaignId)),
      el("td", {}, entryFieldNode(field, rightValue, rightEntry, rightSeed, campaignId)),
    );
  });

  const notes = [];
  if (first.kind === "length") {
    notes.push(
      "Positions after the shorter trace cannot be aligned, so no virtual time can be compared there.",
    );
  } else if (comparison.firstAtDivergence) {
    const at = comparison.firstAtDivergence;
    if (at.index === first.index && first.field !== "at_ns") {
      notes.push(
        `Virtual time also differs at this pair: left ${formatAt(at.leftAt)}, right ${formatAt(at.rightAt)}.`,
      );
    } else if (at.index !== first.index) {
      notes.push(
        `The first virtual-time divergence is later, at position ${formatCount(at.index + 1)}: ` +
          `left ${formatAt(at.leftAt)}, right ${formatAt(at.rightAt)}.`,
      );
    } else {
      notes.push("The first difference is virtual time.");
    }
  } else {
    notes.push("Virtual time is identical at every aligned position.");
  }

  return el(
    "div",
    {},
    notice(
      "warn",
      "First event divergence",
      `${headline} ${formatCount(comparison.commonPrefix)} leading aligned decision(s) match; ` +
        `${formatCount(comparison.differing)} aligned position(s) differ` +
        (comparison.unaligned > 0
          ? `; ${formatCount(comparison.unaligned)} recorded position(s) remain unpaired.`
          : "."),
    ),
    el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data compare-table data--compare-events" },
        el("caption", { class: "sr-only" }, "First differing canonical trace entry"),
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col" }, "Field"),
            el("th", { scope: "col" }, "Left"),
            el("th", { scope: "col" }, "Right"),
          ),
        ),
        el("tbody", {}, ...rows),
      ),
    ),
    el("p", { class: "compare-note" }, notes.join(" ")),
  );
}

function reportCallout(first) {
  return el(
    "div",
    { class: "compare-callout" },
    el(
      "strong",
      { class: "compare-callout__title" },
      `First differing report field: ${first.group} · ${first.label}`,
    ),
    el(
      "div",
      { class: "compare-callout__pair" },
      el(
        "div",
        {},
        el("span", { class: "compare-callout__label" }, "Left"),
        rowValueNode(first, first.left),
      ),
      el(
        "div",
        {},
        el("span", { class: "compare-callout__label" }, "Right"),
        rowValueNode(first, first.right),
      ),
    ),
    el(
      "p",
      { class: "compare-note" },
      `${rowValueText(first, first.left)} → ${rowValueText(first, first.right)}. ` +
        "Rows use the fixed report order documented in ui/README.md; wall time and trace hashes are excluded.",
    ),
  );
}

function reportSummaryTable(comparison) {
  const rows = comparison.rows.filter((row) => row.group === "Report");
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: "data compare-table data--compare-summary" },
      el("caption", { class: "sr-only" }, "Side-by-side report summary"),
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          el("th", { scope: "col" }, "Report field"),
          el("th", { scope: "col" }, "Left"),
          el("th", { scope: "col" }, "Right"),
        ),
      ),
      el(
        "tbody",
        {},
        ...rows.map((row) =>
          el(
            "tr",
            { dataset: { different: String(row.different) } },
            el("th", { scope: "row" }, row.label),
            el("td", {}, rowValueNode(row, row.left)),
            el("td", {}, rowValueNode(row, row.right)),
          ),
        ),
      ),
    ),
  );
}

function reportDiffTable(differentRows) {
  if (differentRows.length === 0) {
    return emptyState({
      title: "No report differences",
      body: "Every compared report field matches. Wall duration is host metadata and is not compared.",
    });
  }
  const shown = differentRows.slice(0, MAX_RENDERED_DIFFS);
  const wrapper = el("div", {});
  wrapper.append(
    el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data compare-table data--compare-report-diff" },
        el("caption", { class: "sr-only" }, "Every differing report field"),
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col" }, "Group"),
            el("th", { scope: "col" }, "Field"),
            el("th", { scope: "col" }, "Left"),
            el("th", { scope: "col" }, "Right"),
          ),
        ),
        el(
          "tbody",
          {},
          ...shown.map((row) =>
            el(
              "tr",
              {},
              el("td", { class: "text-soft" }, row.group),
              el("td", { class: "diff-path" }, row.label),
              el("td", {}, rowValueNode(row, row.left)),
              el("td", {}, rowValueNode(row, row.right)),
            ),
          ),
        ),
      ),
    ),
  );
  if (differentRows.length > shown.length) {
    wrapper.append(
      notice(
        "info",
        "Differences truncated by the UI",
        `Showing the first ${formatCount(shown.length)} of ${formatCount(
          differentRows.length,
        )} differing report fields. The artifact still contains every value.`,
      ),
    );
  }
  return wrapper;
}

function configDiffCell(diff, side) {
  const value = diff[side];
  if (diff.kind === "length") {
    return el("span", { class: "mono" }, `${formatCount(value)} items`);
  }
  if (value === undefined) return absentNode();
  return previewJsonNode(value);
}

function configDiffKind(kind) {
  switch (kind) {
    case "change":
      return "value";
    case "length":
      return "length";
    case "missing":
      return "absent on one side";
    case "type":
      return "type";
    default:
      return String(kind);
  }
}

function configCallout(diff) {
  return el(
    "div",
    { class: "compare-callout" },
    el(
      "strong",
      { class: "compare-callout__title" },
      `First differing config field: ${diff.path || "(root)"}`,
    ),
    el(
      "div",
      { class: "compare-callout__pair" },
      el(
        "div",
        {},
        el("span", { class: "compare-callout__label" }, "Left"),
        configDiffCell(diff, "left"),
      ),
      el(
        "div",
        {},
        el("span", { class: "compare-callout__label" }, "Right"),
        configDiffCell(diff, "right"),
      ),
    ),
    el("p", { class: "compare-note" }, `Difference kind: ${configDiffKind(diff.kind)}.`),
  );
}

function configDiffTable(comparison) {
  const diffs = comparison.diffs;
  if (diffs.length === 0) {
    return emptyState({
      title: "No realized config differences",
      body: "The two realized config objects are equal key by key in the compared portion.",
    });
  }
  const shown = diffs.slice(0, MAX_RENDERED_DIFFS);
  const wrapper = el("div", {});
  wrapper.append(
    el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data compare-table data--compare-config-diff" },
        el("caption", { class: "sr-only" }, "Differing realized config fields"),
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col" }, "Path"),
            el("th", { scope: "col" }, "Left"),
            el("th", { scope: "col" }, "Right"),
            el("th", { scope: "col" }, "Kind"),
          ),
        ),
        el(
          "tbody",
          {},
          ...shown.map((diff) =>
            el(
              "tr",
              {},
              el("td", { class: "diff-path" }, diff.path || "(root)"),
              el("td", {}, configDiffCell(diff, "left")),
              el("td", {}, configDiffCell(diff, "right")),
              el("td", { class: "diff-kind" }, configDiffKind(diff.kind)),
            ),
          ),
        ),
      ),
    ),
  );
  if (diffs.length > shown.length) {
    wrapper.append(
      notice(
        "info",
        "Config differences truncated by the UI",
        `Showing the first ${formatCount(shown.length)} of ${formatCount(
          diffs.length,
        )} collected config differences.`,
      ),
    );
  }
  if (comparison.truncated) {
    wrapper.append(
      notice(
        "info",
        "Config comparison stopped at its bound",
        `${formatCount(comparison.compared)} config nodes were examined` +
          (comparison.skippedDepth > 0
            ? `; ${formatCount(comparison.skippedDepth)} deeper branch(es) were not inspected.`
            : "; the diff stopped after reaching its difference limit."),
      ),
    );
  }
  return wrapper;
}

function selectorPanel({
  campaignId,
  rows,
  leftSeed,
  rightSeed,
  sameSeed,
}) {
  const leftSelect = el(
    "select",
    { id: "compare-left", name: "left" },
    ...rows.map((row) =>
      el(
        "option",
        { value: row.seed },
        `#${formatCount(row.index)} · ${statusLabel(row.status)} · ${row.seed}`,
      ),
    ),
  );
  leftSelect.value = leftSeed;
  const rightSelect = el(
    "select",
    { id: "compare-right", name: "right" },
    ...rows.map((row) =>
      el(
        "option",
        { value: row.seed },
        `#${formatCount(row.index)} · ${statusLabel(row.status)} · ${row.seed}`,
      ),
    ),
  );
  rightSelect.value = rightSeed;
  const go = () => {
    navigate(compareHref(campaignId, leftSelect.value, rightSelect.value), {
      replace: true,
    });
  };
  leftSelect.addEventListener("change", go);
  rightSelect.addEventListener("change", go);
  const swap = button(
    "Swap sides",
    () =>
      navigate(compareHref(campaignId, rightSelect.value, leftSelect.value), {
        replace: true,
      }),
    { id: "compare-swap", class: "btn btn--ghost", disabled: sameSeed },
  );

  return panel({
    title: "Compare selection",
    subtitle:
      "Both universes must belong to this campaign. The query string is the shareable identity.",
    flush: true,
    body: el(
      "div",
      {},
      el(
        "div",
        { class: "compare-toolbar" },
        el(
          "div",
          { class: "field field--grow" },
          el("label", { for: "compare-left" }, "Left universe (seed)"),
          leftSelect,
        ),
        el(
          "div",
          { class: "field field--grow" },
          el("label", { for: "compare-right" }, "Right universe (seed)"),
          rightSelect,
        ),
        swap,
      ),
      el(
        "p",
        { class: "compare-note compare-selector-note" },
        "Seeds are full 64-bit strings and are never formatted through a number. " +
          "Selecting a universe updates the URL; no artifact is modified.",
      ),
    ),
  });
}

function resolveRequestedSeed(raw, fallback, label, rows, notices) {
  if (raw === null) return fallback;
  if (raw.trim() === "") {
    notices.push(
      notice(
        "warn",
        `Empty ${label.toLowerCase()} seed in link`,
        `The ${label.toLowerCase()} parameter is empty. Showing ${fallback} instead.`,
      ),
    );
    return fallback;
  }
  if (canonicalSeed(raw) === null) {
    notices.push(
      notice(
        "warn",
        `Invalid ${label.toLowerCase()} seed in link`,
        `The ${label.toLowerCase()} parameter ${JSON.stringify(
          String(raw).slice(0, 64),
        )} is not a 64-bit decimal or 0x-prefixed seed. Showing ${fallback} instead.`,
      ),
    );
    return fallback;
  }
  const row = rows.find((item) => seedsMatch(item.seed, raw));
  if (!row) {
    notices.push(
      notice(
        "warn",
        `${label} seed not in this campaign`,
        `Seed ${String(raw).slice(0, 64)} has no universe row in this campaign. Showing ${fallback} instead.`,
      ),
    );
    return fallback;
  }
  return row.seed;
}

export async function compareView({ campaignId, signal }) {
  const viewPath = window.location.pathname;
  const data = await api.campaign(campaignId, signal);
  const campaign = data.campaign ?? {};
  const manifest = data.manifest ?? {};
  const rows = (Array.isArray(data.universes) ? data.universes : []).filter(
    (row) => row && typeof row === "object" && typeof row.seed === "string",
  );
  setBuildContext(buildContext(manifest));

  if (rows.length === 0) {
    return {
      title: `Compare · ${campaign.name || campaignId} · Cosmos`,
      node: el(
        "div",
        { class: "page" },
        pageHeader({
          eyebrow: `Campaign / ${campaignId} / compare`,
          title: "Compare universes",
          subtitle: campaign.name || campaignId,
          crumbs: [
            { label: "Campaigns", href: "/" },
            { label: campaign.name || campaignId, href: campaignHref(campaignId) },
            { label: "Universes", href: `${campaignHref(campaignId)}/universes` },
            { label: "Compare" },
          ],
          badges: buildBadges(manifest),
          actions: [
            link(campaignHref(campaignId), "Campaign cockpit", { class: "btn btn--primary" }),
          ],
        }),
        emptyState({
          title: "No universes to compare",
          body: "This campaign artifact has no universe rows, so there is nothing to compare.",
        }),
      ),
    };
  }

  const params = new URLSearchParams(window.location.search);
  const notices = [];
  const defaultLeft = rows[0].seed;
  const leftSeed = resolveRequestedSeed(
    params.get("left"),
    defaultLeft,
    "Left",
    rows,
    notices,
  );
  const defaultRight =
    rows.find((row) => !seedsMatch(row.seed, leftSeed))?.seed ?? leftSeed;
  const rightSeed = resolveRequestedSeed(
    params.get("right"),
    defaultRight,
    "Right",
    rows,
    notices,
  );
  const sameSeed = seedsMatch(leftSeed, rightSeed);

  const canonical = compareHref(campaignId, leftSeed, rightSeed);
  const current = `${window.location.pathname}${window.location.search}`;
  if (current !== canonical && window.location.pathname === viewPath) {
    window.history.replaceState(null, "", canonical);
  }

  if (sameSeed) {
    notices.push(
      notice(
        "warn",
        "Both sides select the same universe",
        `The comparison is trivially identical. Choose a different seed on either side to compare behaviour.`,
      ),
    );
  }

  const uniqueSeeds = [...new Set([leftSeed, rightSeed])];
  const details = new Map();
  const traceSides = new Map();
  await Promise.all(
    uniqueSeeds.map(async (seed) => {
      const [universeResult, traceResult] = await Promise.allSettled([
        api.universe(campaignId, seed, signal),
        api.trace(campaignId, seed, signal),
      ]);
      if (universeResult.status === "rejected") throw universeResult.reason;
      if (
        traceResult.status === "rejected" &&
        (traceResult.reason?.name === "AbortError" || signal?.aborted)
      ) {
        throw traceResult.reason;
      }
      details.set(seed, universeResult.value);
      traceSides.set(
        seed,
        traceResult.status === "fulfilled"
          ? { trace: traceResult.value.trace ?? null, error: null }
          : { trace: null, error: traceResult.reason },
      );
    }),
  );

  const leftData = details.get(leftSeed);
  const rightData = details.get(rightSeed);
  if (!leftData || !rightData) {
    throw new Error("The selected universes could not be resolved.");
  }
  const leftUniverse = leftData.universe ?? {};
  const rightUniverse = rightData.universe ?? {};
  const leftTraceSide = traceSides.get(leftSeed);
  const rightTraceSide = traceSides.get(rightSeed);
  const leftTrace = leftTraceSide?.trace ?? null;
  const rightTrace = rightTraceSide?.trace ?? null;
  const bothTracePresent =
    !leftTraceSide?.error &&
    !rightTraceSide?.error &&
    leftTrace?.present === true &&
    rightTrace?.present === true;
  const eventComparable =
    bothTracePresent &&
    leftTrace?.seed_matches !== false &&
    rightTrace?.seed_matches !== false &&
    Number.isInteger(leftTrace?.dropped) &&
    leftTrace.dropped === 0 &&
    Number.isInteger(rightTrace?.dropped) &&
    rightTrace.dropped === 0;
  const traceIdentityMismatch =
    leftTrace?.seed_matches === false || rightTrace?.seed_matches === false;
  const dropCountUndeclared =
    bothTracePresent &&
    (!Number.isInteger(leftTrace?.dropped) || !Number.isInteger(rightTrace?.dropped));
  const traceTruncated =
    bothTracePresent &&
    !dropCountUndeclared &&
    (leftTrace.dropped > 0 || rightTrace.dropped > 0);
  const leftEntries = Array.isArray(leftTrace?.entries)
    ? leftTrace.entries.filter((entry) => entry && typeof entry === "object")
    : [];
  const rightEntries = Array.isArray(rightTrace?.entries)
    ? rightTrace.entries.filter((entry) => entry && typeof entry === "object")
    : [];
  const configComparison = compareConfigs(leftUniverse.config, rightUniverse.config);
  const reportComparison = compareReports(leftUniverse, rightUniverse);
  const traceComparison = eventComparable
    ? compareTraceEntries(leftEntries, rightEntries)
    : null;

  const sides = [
    { label: "Left", seed: leftSeed, data: leftData, traceSide: leftTraceSide },
  ];
  if (!sameSeed) {
    sides.push({
      label: "Right",
      seed: rightSeed,
      data: rightData,
      traceSide: rightTraceSide,
    });
  }
  for (const side of sides) {
    const universe = side.data.universe ?? {};
    const traceSide = side.traceSide;
    if (traceSide?.error) {
      notices.push(
        notice(
          "bad",
          `${side.label} trace could not be loaded`,
          traceSide.error.message ||
            "The canonical trace request failed. Report and config comparison are still available.",
        ),
      );
    } else if (traceSide?.trace && !traceSide.trace.present) {
      const trace = traceSide.trace;
      notices.push(
        notice(
          "warn",
          `${side.label} trace unavailable`,
          trace.missing
            ? "The artifact metadata says a trace was captured, but the trace file is absent. Trace comparison is unavailable for this side."
            : trace.traced
              ? "This universe was traced, but no canonical bytes were exported. Trace comparison is unavailable for this side."
              : "This campaign did not capture canonical trace bytes for this universe. Trace comparison is unavailable for this side.",
        ),
      );
    }
    if (traceSide?.trace?.present === true && !Number.isInteger(traceSide.trace.dropped)) {
      notices.push(
        notice(
          "warn",
          `${side.label} trace drop count not declared`,
          "The trace file is present, but the artifact does not declare an integer dropped count. " +
            "Event comparison is disabled for this pair.",
        ),
      );
    }
    if (
      traceSide?.trace?.present === true &&
      Number(traceSide.trace.dropped) > 0
    ) {
      notices.push(
        notice(
          "warn",
          `${side.label} trace truncated`,
          `The fixed-capacity buffer dropped ${formatCount(
            traceSide.trace.dropped,
          )} decisions. Event comparison is disabled; the trace summary covers the recorded prefix only.`,
        ),
      );
    }
    if (traceSide?.trace?.hash_matches === false) {
      notices.push(
        notice(
          "bad",
          `${side.label} trace hash mismatch`,
          `The canonical bytes hash to ${traceSide.trace.computed_hash}, but the artifact declares ${traceSide.trace.declared_hash}. Treat this trace as unverified evidence.`,
        ),
      );
    }
    if (traceSide?.trace?.seed_matches === false) {
      notices.push(
        notice(
          "bad",
          `${side.label} trace seed mismatch`,
          `The trace header names seed ${traceSide.trace.seed}, but this universe is seed ${universe.seed}. The trace does not belong to this universe identity.`,
        ),
      );
    }
    if (
      universe.verify_failed === true ||
      universe.status === "verify_failed"
    ) {
      notices.push(
        notice(
          "verify",
          `Verification failed in the ${side.label.toLowerCase()} universe`,
          universe.verify_mismatch ||
            "Two runs of this seed did not agree. Treat the run as unverified evidence.",
        ),
      );
    }
  }

  if (
    configComparison.leftPresent &&
    configComparison.rightPresent &&
    configComparison.complete &&
    typeof leftUniverse.config_hash === "string" &&
    typeof rightUniverse.config_hash === "string"
  ) {
    const hashesEqual = leftUniverse.config_hash === rightUniverse.config_hash;
    if (hashesEqual !== configComparison.equal) {
      notices.push(
        notice(
          "warn",
          hashesEqual
            ? "Config hash matches but realized configs differ"
            : "Config hash differs but realized configs match",
          "The artifact's config hash and realized config object disagree. Treat the hash as unverified for this pair.",
        ),
      );
    }
  }

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId} / compare`,
    title: "Compare universes",
    subtitle:
      `${campaign.name || campaignId} · ${formatCount(rows.length)} universes · ` +
      "read-only report, config and canonical-trace comparison",
    crumbs: [
      { label: "Campaigns", href: "/" },
      { label: campaign.name || campaignId, href: campaignHref(campaignId) },
      { label: "Universes", href: `${campaignHref(campaignId)}/universes` },
      { label: "Compare" },
    ],
    badges: [
      ...buildBadges(manifest),
      sameSeed ? badge("same seed", "warn") : null,
    ].filter(Boolean),
    actions: [
      link(campaignHref(campaignId), "Campaign cockpit", { class: "btn btn--ghost" }),
      link(`${campaignHref(campaignId)}/universes`, "Universes", {
        class: "btn btn--ghost",
      }),
      copyButton(() => window.location.href, "Copy compare link", {
        class: "btn btn--ghost",
      }),
    ],
  });

  const leftPanel = sidePanel({
    label: "Left",
    campaignId,
    universeData: leftData,
    traceSide: leftTraceSide,
    campaign,
  });
  const rightPanel = sidePanel({
    label: "Right",
    campaignId,
    universeData: rightData,
    traceSide: rightTraceSide,
    campaign,
  });

  const traceBody = el("div", {});
  if (bothTracePresent) {
    traceBody.append(traceSummaryTable(compareTraceSummaries(leftTrace, rightTrace)));
    if (eventComparable) {
      traceBody.append(
        el(
          "div",
          { class: "section-gap" },
          eventDivergenceBody({
            comparison: traceComparison,
            leftEntries,
            rightEntries,
            leftSeed,
            rightSeed,
            campaignId,
          }),
        ),
      );
    } else {
      traceBody.append(
        notice(
          "warn",
          traceIdentityMismatch
            ? "Event comparison disabled: trace identity mismatch"
            : dropCountUndeclared
              ? "Event comparison disabled: drop count not declared"
              : traceTruncated
                ? "Event comparison disabled: trace truncated"
                : "Event comparison disabled",
          traceIdentityMismatch
            ? "A trace header seed does not match its universe row. The entries are summarized above but are not paired, because the bytes do not belong to the universe identity being compared."
            : dropCountUndeclared
              ? "The trace file is present, but at least one artifact does not declare an integer dropped count. The recorded entries are summarized above but are not treated as complete evidence."
              : "At least one trace reports dropped decisions. The recorded prefix is summarized above but is not treated as complete evidence, so no first event divergence is claimed.",
        ),
      );
    }
    traceBody.append(
      el(
        "p",
        { class: "compare-note" },
        sameSeed
          ? "Same seed: the canonical trace bytes and hash should be identical."
          : eventComparable
            ? "A differing trace hash is expected across seeds: trace_hash() covers the canonical header, " +
                "which includes the universe seed. The event comparison above uses only the frozen entry fields."
            : "A differing trace hash is expected across seeds: trace_hash() covers the canonical header, " +
                "which includes the universe seed. The trace summary above uses only the recorded fields; event comparison is disabled.",
      ),
    );
  } else {
    traceBody.append(
      notice(
        "warn",
        "Trace comparison unavailable",
        "Both universes need a readable canonical trace before entries can be compared. " +
          "The report and config comparison below still use the universe documents.",
      ),
    );
    traceBody.append(
      el(
        "div",
        { class: "grid-2 section-gap" },
        traceMetaSide("Left trace", leftTraceSide),
        traceMetaSide("Right trace", rightTraceSide),
      ),
    );
    if (
      leftTrace?.declared_hash ||
      rightTrace?.declared_hash ||
      leftTrace?.computed_hash ||
      rightTrace?.computed_hash
    ) {
      traceBody.append(
        el(
          "p",
          { class: "compare-note" },
          "A trace hash difference is identity, not behaviour: trace_hash() covers the canonical " +
            "header, including the seed. No event comparison is available for this pair.",
        ),
      );
    }
  }
  const tracePanel = panel({
    title: "Canonical trace",
    subtitle: "Pairwise entry comparison in canonical encoding order",
    count: bothTracePresent
      ? `${leftEntries.length} / ${rightEntries.length}`
      : undefined,
    body: traceBody,
  });

  const reportBody = el("div", {});
  if (reportComparison.first) {
    reportBody.append(reportCallout(reportComparison.first));
  } else {
    reportBody.append(
      notice(
        "info",
        "No report differences",
        `${formatCount(reportComparison.comparedCount)} report fields match. ` +
          "Wall duration is host metadata and is excluded from the comparison.",
      ),
    );
  }
  reportBody.append(
    el("div", { class: "section-gap" }, reportSummaryTable(reportComparison)),
  );
  const ledgerDropped =
    Number(leftUniverse.ledger_dropped ?? 0) + Number(rightUniverse.ledger_dropped ?? 0);
  if (ledgerDropped > 0) {
    reportBody.append(
      notice(
        "warn",
        "Ledger comparison uses recorded rows only",
        `The artifact dropped ${formatCount(leftUniverse.ledger_dropped ?? 0)} ledger row(s) on the left ` +
          `and ${formatCount(rightUniverse.ledger_dropped ?? 0)} on the right. ` +
          "A matching recorded prefix is not proof that the unrecorded tails matched.",
      ),
    );
  }
  reportBody.append(
    el("h3", { class: "compare-section-title section-gap" }, "All report differences"),
  );
  reportBody.append(reportDiffTable(reportComparison.differentRows));
  const reportPanel = panel({
    title: "Report",
    subtitle:
      "First differing field in the fixed report order; wall time and trace hashes are excluded",
    count: reportComparison.differentCount,
    body: reportBody,
  });

  const configBody = el("div", {});
  if (!configComparison.leftPresent && !configComparison.rightPresent) {
    configBody.append(
      emptyState({
        title: "No realized config",
        body: "Neither universe captured an injector configuration object.",
      }),
    );
  } else {
    if (configComparison.equal) {
      if (configComparison.complete) {
        configBody.append(
          notice(
            "info",
            "No realized config differences",
            `${formatCount(configComparison.compared)} config nodes compared. ` +
              "The per-universe config object is what each seed actually ran (D14), not the campaign template.",
          ),
        );
      } else {
        configBody.append(
          notice(
            "warn",
            "Config comparison incomplete",
            `${formatCount(configComparison.compared)} config nodes were examined without a difference, ` +
              `${formatCount(configComparison.skippedDepth)} deeper branch(es) were not inspected.`,
          ),
        );
      }
    } else {
      configBody.append(configCallout(configComparison.first));
      configBody.append(configDiffTable(configComparison));
    }
    if (configComparison.leftPresent) {
      configBody.append(
        el(
          "div",
          { class: "section-gap" },
          detailsBlock(
            "Left realized config JSON",
            el("pre", { class: "code-block" }, JSON.stringify(leftUniverse.config, null, 2)),
          ),
        ),
      );
    }
    if (configComparison.rightPresent) {
      configBody.append(
        el(
          "div",
          { class: "section-gap" },
          detailsBlock(
            "Right realized config JSON",
            el("pre", { class: "code-block" }, JSON.stringify(rightUniverse.config, null, 2)),
          ),
        ),
      );
    }
  }
  const configPanel = panel({
    title: "Realized configuration",
    subtitle: "Compared key by key; the config hash is the artifact's fingerprint",
    count: configComparison.diffs.length,
    body: configBody,
  });

  return {
    title: `Compare · ${campaign.name || campaignId} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      header,
      notices.length > 0 ? el("div", { class: "stack" }, ...notices) : null,
      selectorPanel({ campaignId, rows, leftSeed, rightSeed, sameSeed }),
      el("div", { class: "grid-2 section-gap" }, leftPanel, rightPanel),
      el("div", { class: "section-gap" }, tracePanel),
      el("div", { class: "section-gap" }, reportPanel),
      el("div", { class: "section-gap" }, configPanel),
    ),
  };
}
