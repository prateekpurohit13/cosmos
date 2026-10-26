// Deterministic, read-only comparison for two schema-v1 universe documents and their canonical
// fault traces (docs/ui-design.md U2.3). Nothing here invents engine data, sorts by wall time, or
// mutates an artifact. Seeds stay strings so a 64-bit identity never goes through a lossy number
// conversion (D9, D12).
//
// Two rules are load-bearing:
//
// * Canonical trace entries are compared pairwise in the frozen encoding order. A trace hash is
//   not used as a divergence signal because the canonical header includes the universe seed; two
//   universes that made identical decisions still hash differently.
// * Report fields use one fixed order below, and wall-clock time is never part of it (D10).

import { compareSites } from "./faults.js";
import { laneOrder, traceCounts } from "./trace.js";

export const TRACE_COMPARE_FIELDS = [
  ["seq", "Event sequence"],
  ["eligible_index", "Eligible call"],
  ["at_ns", "Virtual time"],
  ["site", "Site"],
  ["amount_ns", "Amount"],
  ["outcome", "Outcome"],
  ["drew", "Drew"],
  ["status", "Status"],
];

const SCALAR_REPORT_FIELDS = [
  ["status", "Status", "status"],
  ["virtual_end_ns", "Virtual end", "time"],
  ["no_check_failed", "No check failed", "bool"],
  ["vacuous", "Vacuous", "bool"],
  ["verify_failed", "Verify failed", "bool"],
  ["verify_mismatch", "Verify mismatch", "text"],
  ["died", "Process died", "bool"],
  ["signal", "Signal", "count"],
  ["threw", "Threw", "text"],
  ["active_allocations", "Active allocations", "count"],
  ["out_of_scope_assertions", "Out of scope assertions", "count"],
  ["ledger_dropped", "Ledger dropped", "count"],
  ["config_hash", "Config hash", "hash"],
  ["trace.present", "Trace metadata present", "bool"],
  ["trace.traced", "Trace metadata captured", "bool"],
  ["trace.dropped", "Trace dropped", "count"],
];

const LEDGER_FIELDS = [
  "fire_index",
  "at_ns",
  "site",
  "eligible_index",
  "fault_class",
  "outcome",
  "drew",
  "status",
  "reason",
  "persistent",
];

function compareStrings(left, right) {
  const l = String(left ?? "");
  const r = String(right ?? "");
  return l < r ? -1 : l > r ? 1 : 0;
}

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function hasOwn(object, key) {
  return Boolean(object) && Object.prototype.hasOwnProperty.call(object, key);
}

function objectValue(object, key) {
  return hasOwn(object, key) ? object[key] : undefined;
}

function traceValue(trace, key) {
  return objectValue(trace, key);
}

function sameValue(left, right) {
  return left === right;
}

function filterEntries(entries) {
  return Array.isArray(entries)
    ? entries.filter((entry) => entry && typeof entry === "object")
    : [];
}

export function canonicalSeed(value) {
  if (typeof value !== "string") return null;
  const text = value.trim();
  // A 64-bit decimal seed is at most 20 digits and a 0x seed at most 18 characters. Refusing a
  // longer token keeps a hostile query string from making BigInt parse an enormous integer.
  if (!text || text.length > 32) return null;
  if (!/^(?:0[xX][0-9a-fA-F]+|[0-9]+)$/.test(text)) return null;
  try {
    const parsed = BigInt(text);
    if (parsed > (1n << 64n) - 1n) return null;
    return parsed.toString();
  } catch {
    return null;
  }
}

export function seedsMatch(left, right) {
  const l = canonicalSeed(left);
  const r = canonicalSeed(right);
  return l !== null && r !== null && l === r;
}

// The server supplies both the enum token and its numeric id. The token is what the UI shows;
// the id is a fallback for a hand-written or older artifact, never a new source of truth.
export function traceEntryField(entry, field) {
  if (!entry || typeof entry !== "object") return null;
  if (field === "site") {
    if (entry.site !== undefined && entry.site !== null && String(entry.site) !== "") {
      return String(entry.site);
    }
    return entry.site_id === undefined || entry.site_id === null
      ? null
      : `site#${entry.site_id}`;
  }
  if (field === "outcome") {
    if (entry.outcome !== undefined && entry.outcome !== null && String(entry.outcome) !== "") {
      return String(entry.outcome);
    }
    return entry.outcome_id === undefined || entry.outcome_id === null
      ? null
      : `outcome#${entry.outcome_id}`;
  }
  if (field === "status") {
    if (entry.status !== undefined && entry.status !== null && String(entry.status) !== "") {
      return String(entry.status);
    }
    return entry.status_id === undefined || entry.status_id === null
      ? null
      : `status#${entry.status_id}`;
  }
  return entry[field] === undefined ? null : entry[field];
}

function sameTraceField(left, right, field) {
  return sameValue(traceEntryField(left, field), traceEntryField(right, field));
}

// Pairwise comparison over the canonical sequence. `seq` is the event identity, and the writer
// records a prefix, so equal positions are equal events unless the artifact is malformed.
export function compareTraceEntries(leftEntries, rightEntries) {
  const left = filterEntries(leftEntries);
  const right = filterEntries(rightEntries);
  const aligned = Math.min(left.length, right.length);
  const limit = Math.max(left.length, right.length);
  let first = null;
  let firstAtDivergence = null;
  let differing = 0;
  let unaligned = 0;

  for (let index = 0; index < limit; index += 1) {
    const leftEntry = index < left.length ? left[index] : null;
    const rightEntry = index < right.length ? right[index] : null;
    if (!leftEntry || !rightEntry) {
      unaligned += 1;
      if (!first) {
        first = {
          index,
          kind: "length",
          field: null,
          fieldLabel: null,
          missingSide: leftEntry ? "right" : "left",
          leftSeq: leftEntry ? traceEntryField(leftEntry, "seq") : null,
          rightSeq: rightEntry ? traceEntryField(rightEntry, "seq") : null,
        };
      }
      continue;
    }

    const leftAt = traceEntryField(leftEntry, "at_ns");
    const rightAt = traceEntryField(rightEntry, "at_ns");
    if (firstAtDivergence === null && !sameValue(leftAt, rightAt)) {
      firstAtDivergence = { index, leftAt, rightAt };
    }

    let changed = null;
    for (const [field, label] of TRACE_COMPARE_FIELDS) {
      if (!sameTraceField(leftEntry, rightEntry, field)) {
        changed = { field, label };
        break;
      }
    }
    if (changed) {
      differing += 1;
      if (!first) {
        first = {
          index,
          kind: "entry",
          field: changed.field,
          fieldLabel: changed.label,
          missingSide: null,
          leftSeq: traceEntryField(leftEntry, "seq"),
          rightSeq: traceEntryField(rightEntry, "seq"),
        };
      }
    }
  }

  return {
    alignedCount: aligned,
    commonPrefix: first ? Math.min(first.index, aligned) : aligned,
    equal: differing === 0 && unaligned === 0,
    differing,
    unaligned,
    first,
    firstAtDivergence,
  };
}

function traceSpan(counts) {
  if (counts.minAt === null || counts.maxAt === null) return null;
  return Math.max(0, counts.maxAt - counts.minAt);
}

function classList(counts) {
  return laneOrder(counts).join(", ") || "none";
}

function summaryRow(rows, key, label, left, right, format = "text") {
  rows.push({
    key,
    label,
    left,
    right,
    format,
    different: !sameValue(left, right),
  });
}

// A side-by-side trace summary. Differences are informational here; the event comparison above
// is what defines divergence. In particular a hash difference is expected across seeds.
export function compareTraceSummaries(leftTrace, rightTrace) {
  const leftEntries = filterEntries(traceValue(leftTrace, "entries"));
  const rightEntries = filterEntries(traceValue(rightTrace, "entries"));
  const leftCounts = traceCounts(leftEntries);
  const rightCounts = traceCounts(rightEntries);
  const rows = [];
  summaryRow(rows, "present", "Trace present", traceValue(leftTrace, "present"), traceValue(rightTrace, "present"), "bool");
  summaryRow(rows, "traced", "Trace captured", traceValue(leftTrace, "traced"), traceValue(rightTrace, "traced"), "bool");
  summaryRow(rows, "missing", "File missing", traceValue(leftTrace, "missing"), traceValue(rightTrace, "missing"), "bool");
  summaryRow(rows, "count", "Recorded entries", traceValue(leftTrace, "count"), traceValue(rightTrace, "count"), "count");
  summaryRow(rows, "capacity", "Buffer capacity", traceValue(leftTrace, "capacity"), traceValue(rightTrace, "capacity"), "count");
  summaryRow(rows, "dropped", "Dropped entries", traceValue(leftTrace, "dropped"), traceValue(rightTrace, "dropped"), "count");
  summaryRow(rows, "declared_hash", "Declared hash", traceValue(leftTrace, "declared_hash"), traceValue(rightTrace, "declared_hash"), "hash");
  summaryRow(rows, "computed_hash", "Computed hash", traceValue(leftTrace, "computed_hash"), traceValue(rightTrace, "computed_hash"), "hash");
  summaryRow(rows, "hash_matches", "Hash verified", traceValue(leftTrace, "hash_matches"), traceValue(rightTrace, "hash_matches"), "tri");
  summaryRow(rows, "seed_matches", "Header seed matches", traceValue(leftTrace, "seed_matches"), traceValue(rightTrace, "seed_matches"), "tri");
  summaryRow(rows, "seed", "Header seed", traceValue(leftTrace, "seed"), traceValue(rightTrace, "seed"), "seed");
  summaryRow(rows, "injected", "Injected decisions", leftCounts.injected, rightCounts.injected, "count");
  summaryRow(rows, "no_fault", "Pass-through decisions", leftCounts.noFault, rightCounts.noFault, "count");
  summaryRow(rows, "suppressed", "Suppressed decisions", leftCounts.suppressed, rightCounts.suppressed, "count");
  summaryRow(rows, "skipped", "Skipped decisions", leftCounts.skipped, rightCounts.skipped, "count");
  summaryRow(rows, "classes", "Classes", classList(leftCounts), classList(rightCounts), "text");
  summaryRow(rows, "min_at", "First event time", leftCounts.minAt, rightCounts.minAt, "time");
  summaryRow(rows, "max_at", "Last event time", leftCounts.maxAt, rightCounts.maxAt, "time");
  summaryRow(rows, "span", "Virtual span", traceSpan(leftCounts), traceSpan(rightCounts), "duration");
  return rows;
}

function sortedChecks(universe) {
  const checks = Array.isArray(universe?.checks)
    ? universe.checks.filter((check) => check && typeof check === "object")
    : [];
  return checks
    .map((check, order) => ({
      id: String(check.id ?? ""),
      atNs: check.at_ns,
      passed: check.passed,
      detail: check.detail,
      order,
    }))
    .sort((left, right) => {
      const byId = compareStrings(left.id, right.id);
      if (byId !== 0) return byId;
      const byTime = Number(left.atNs ?? 0) - Number(right.atNs ?? 0);
      if (Number.isFinite(byTime) && byTime !== 0) return byTime;
      const byPassed = Number(left.passed === true) - Number(right.passed === true);
      if (byPassed !== 0) return byPassed;
      const byDetail = compareStrings(left.detail, right.detail);
      if (byDetail !== 0) return byDetail;
      return left.order - right.order;
    });
}

function groupChecks(checks) {
  const groups = new Map();
  for (const check of checks) {
    const group = groups.get(check.id);
    if (group) group.push(check);
    else groups.set(check.id, [check]);
  }
  return groups;
}

function checkTupleKey(check) {
  return JSON.stringify([check.passed, check.atNs, check.detail]);
}

// Duplicate assertion ids are legitimate. Match identical occurrences first, then pair the
// remaining values in deterministic order and report leftovers as extra/missing. This avoids
// cross-pairing two identical occurrences with two different ones just because their ordinals
// happen to line up after sorting.
function pairChecks(leftChecks, rightChecks) {
  const rightByKey = new Map();
  rightChecks.forEach((check, index) => {
    const key = checkTupleKey(check);
    const queue = rightByKey.get(key);
    if (queue) queue.push(index);
    else rightByKey.set(key, [index]);
  });
  const usedRight = new Set();
  const leftRemaining = [];
  const pairs = [];
  for (const left of leftChecks) {
    const queue = rightByKey.get(checkTupleKey(left));
    if (queue && queue.length > 0) {
      const index = queue.shift();
      usedRight.add(index);
      pairs.push([left, rightChecks[index]]);
    } else {
      leftRemaining.push(left);
    }
  }
  const rightRemaining = rightChecks.filter((_, index) => !usedRight.has(index));
  const common = Math.min(leftRemaining.length, rightRemaining.length);
  for (let index = 0; index < common; index += 1) {
    pairs.push([leftRemaining[index], rightRemaining[index]]);
  }
  for (let index = common; index < leftRemaining.length; index += 1) {
    pairs.push([leftRemaining[index], null]);
  }
  for (let index = common; index < rightRemaining.length; index += 1) {
    pairs.push([null, rightRemaining[index]]);
  }
  return pairs;
}

function coverageMap(universe) {
  const map = new Map();
  const notes = Array.isArray(universe?.coverage)
    ? universe.coverage.filter((note) => note && typeof note === "object")
    : [];
  for (const note of notes) {
    const id = String(note.id ?? "");
    // Duplicate ids are not part of the schema; keeping the first occurrence makes the view
    // deterministic without inventing a merged value.
    if (!map.has(id)) map.set(id, note);
  }
  return map;
}

function siteMap(universe) {
  const sites = isObject(universe?.sites) ? universe.sites : {};
  return new Map(Object.keys(sites).map((site) => [site, isObject(sites[site]) ? sites[site] : {}]));
}

function ledgerList(universe) {
  return Array.isArray(universe?.ledger)
    ? universe.ledger.filter((entry) => entry && typeof entry === "object")
    : [];
}

function ledgerSignature(entry) {
  return JSON.stringify(LEDGER_FIELDS.map((field) => (hasOwn(entry, field) ? entry[field] : null)));
}

function describeLedgerRow(entry) {
  if (!entry) return "absent";
  const raw = (value) => (value === undefined ? "absent" : JSON.stringify(value));
  return [
    `fire_index=${raw(entry.fire_index)}`,
    `at_ns=${raw(entry.at_ns)}`,
    `site=${raw(entry.site)}`,
    `eligible_index=${raw(entry.eligible_index)}`,
    `fault_class=${raw(entry.fault_class)}`,
    `outcome=${raw(entry.outcome)}`,
    `drew=${raw(entry.drew)}`,
    `status=${raw(entry.status)}`,
    `reason=${raw(entry.reason)}`,
    `persistent=${raw(entry.persistent)}`,
  ].join(" ");
}

function firstLedgerDifference(leftLedger, rightLedger) {
  const limit = Math.max(leftLedger.length, rightLedger.length);
  for (let index = 0; index < limit; index += 1) {
    const left = leftLedger[index] ?? null;
    const right = rightLedger[index] ?? null;
    if (!left || !right || ledgerSignature(left) !== ledgerSignature(right)) {
      return { left: describeLedgerRow(left), right: describeLedgerRow(right) };
    }
  }
  return null;
}

function reportRow(rows, key, label, group, left, right, format = "text", options = {}) {
  rows.push({
    key,
    label,
    group,
    left,
    right,
    format,
    different: options.different ?? !sameValue(left, right),
  });
}

// Fixed UI report order, documented in ui/README.md and Appendix A.5.1. It is deliberately not
// the engine's first_report_difference() order: that function exists for same-seed determinism
// and starts with the seed, while this view compares two different seeds and separates config
// and trace metadata from behavioural fields.
export function compareReports(leftUniverse, rightUniverse) {
  const rows = [];

  for (const [key, label, format] of SCALAR_REPORT_FIELDS) {
    const left = key.startsWith("trace.")
      ? traceValue(leftUniverse?.trace, key.slice("trace.".length))
      : objectValue(leftUniverse, key);
    const right = key.startsWith("trace.")
      ? traceValue(rightUniverse?.trace, key.slice("trace.".length))
      : objectValue(rightUniverse, key);
    reportRow(rows, key, label, "Report", left, right, format);
  }

  const leftChecks = sortedChecks(leftUniverse);
  const rightChecks = sortedChecks(rightUniverse);
  reportRow(rows, "checks.count", "Checks (total)", "Checks", leftChecks.length, rightChecks.length, "count");
  reportRow(
    rows,
    "checks.failed",
    "Checks (failed)",
    "Checks",
    leftChecks.filter((check) => check.passed === false).length,
    rightChecks.filter((check) => check.passed === false).length,
    "count",
  );

  const leftCheckMap = groupChecks(leftChecks);
  const rightCheckMap = groupChecks(rightChecks);
  const checkIds = [...new Set([...leftCheckMap.keys(), ...rightCheckMap.keys()])].sort(compareStrings);
  for (const id of checkIds) {
    const pairs = pairChecks(leftCheckMap.get(id) ?? [], rightCheckMap.get(id) ?? []);
    pairs.forEach(([left, right], index) => {
      const occurrence = index + 1;
      const suffix = occurrence > 1 ? ` (${occurrence})` : "";
      const prefix = `check ${id || "(no id)"}${suffix}`;
      const key = JSON.stringify([id, occurrence]);
      reportRow(rows, `${key}.passed`, `${prefix}.passed`, "Checks", left?.passed, right?.passed, "bool");
      reportRow(rows, `${key}.at_ns`, `${prefix}.at_ns`, "Checks", left?.atNs, right?.atNs, "time");
      reportRow(rows, `${key}.detail`, `${prefix}.detail`, "Checks", left?.detail, right?.detail, "text");
    });
  }

  const leftCoverage = coverageMap(leftUniverse);
  const rightCoverage = coverageMap(rightUniverse);
  const coverageIds = [...new Set([...leftCoverage.keys(), ...rightCoverage.keys()])].sort(compareStrings);
  for (const id of coverageIds) {
    reportRow(
      rows,
      `coverage.${id}.hit`,
      `coverage ${id || "(no id)"}.hit`,
      "Coverage",
      leftCoverage.get(id)?.hit,
      rightCoverage.get(id)?.hit,
      "bool",
    );
  }

  const leftSites = siteMap(leftUniverse);
  const rightSites = siteMap(rightUniverse);
  const siteIds = [...new Set([...leftSites.keys(), ...rightSites.keys()])].sort(compareSites);
  for (const site of siteIds) {
    const left = leftSites.get(site) ?? {};
    const right = rightSites.get(site) ?? {};
    reportRow(rows, `sites.${site}.activated`, `site ${site}.activated`, "Sites", left.activated, right.activated, "bool");
    reportRow(rows, `sites.${site}.eligible`, `site ${site}.eligible`, "Sites", left.eligible, right.eligible, "count");
    reportRow(rows, `sites.${site}.fired`, `site ${site}.fired`, "Sites", left.fired, right.fired, "count");
  }

  const leftLedger = ledgerList(leftUniverse);
  const rightLedger = ledgerList(rightUniverse);
  const ledgerTruncated =
    Number(leftUniverse?.ledger_dropped ?? 0) > 0 ||
    Number(rightUniverse?.ledger_dropped ?? 0) > 0;
  reportRow(rows, "ledger.rows", "Ledger rows", "Fault ledger", leftLedger.length, rightLedger.length, "count");
  const ledgerDifference = firstLedgerDifference(leftLedger, rightLedger);
  reportRow(
    rows,
    "ledger.first_difference",
    ledgerTruncated
      ? "Ledger (first difference in recorded prefix)"
      : "Ledger (first difference)",
    "Fault ledger",
    ledgerDifference?.left ?? null,
    ledgerDifference?.right ?? null,
    "text",
    { different: ledgerDifference !== null },
  );

  const differentRows = rows.filter((row) => row.different);
  return {
    rows,
    differentRows,
    differentCount: differentRows.length,
    comparedCount: rows.length,
    first: differentRows[0] ?? null,
  };
}

function pushDiff(state, path, kind, left, right, extra = {}) {
  if (state.diffs.length >= state.maxDiffs) {
    state.truncated = true;
    return;
  }
  state.diffs.push({ path, kind, left, right, ...extra });
}

function walkJson(state, path, left, right, depth) {
  if (state.diffs.length >= state.maxDiffs) {
    state.truncated = true;
    return;
  }
  if (depth > state.maxDepth) {
    // The comparison cannot inspect deeper values. Mark the scan incomplete rather than
    // reporting a false difference for a pair that may be identical below this level.
    state.skippedDepth += 1;
    state.truncated = true;
    return;
  }
  state.compared += 1;

  if (left === undefined && right === undefined) return;
  if (left === undefined || right === undefined) {
    pushDiff(state, path || "config", "missing", left, right, {
      missingSide: left === undefined ? "left" : "right",
    });
    return;
  }

  const leftArray = Array.isArray(left);
  const rightArray = Array.isArray(right);
  if (leftArray && rightArray) {
    if (left.length !== right.length) {
      pushDiff(state, path || "config", "length", left.length, right.length);
    }
    const common = Math.min(left.length, right.length);
    for (let index = 0; index < common; index += 1) {
      walkJson(state, path ? `${path}[${index}]` : `[${index}]`, left[index], right[index], depth + 1);
    }
    return;
  }

  if (leftArray !== rightArray) {
    pushDiff(state, path || "config", "type", left, right);
    return;
  }

  if (isObject(left) && isObject(right)) {
    const keys = [...new Set([...Object.keys(left), ...Object.keys(right)])].sort(compareStrings);
    for (const key of keys) {
      walkJson(state, path ? `${path}.${key}` : key, left[key], right[key], depth + 1);
    }
    return;
  }

  if (isObject(left) !== isObject(right)) {
    pushDiff(state, path || "config", "type", left, right);
    return;
  }

  if (!sameValue(left, right)) {
    pushDiff(state, path || "config", "change", left, right);
  }
}

// Bounded recursive JSON diff. The bound is explicit in the result rather than silently dropping
// evidence; the UI says when it truncated.
export function diffJsonValues(left, right, { maxDepth = 32, maxDiffs = 500 } = {}) {
  const state = {
    diffs: [],
    truncated: false,
    skippedDepth: 0,
    compared: 0,
    maxDepth: Number.isSafeInteger(maxDepth) ? maxDepth : 32,
    maxDiffs: Number.isSafeInteger(maxDiffs) ? maxDiffs : 500,
  };
  walkJson(state, "", left, right, 0);
  return {
    diffs: state.diffs,
    truncated: state.truncated,
    skippedDepth: state.skippedDepth,
    compared: state.compared,
    equal: state.diffs.length === 0,
    complete: !state.truncated,
    first: state.diffs[0] ?? null,
  };
}

export function compareConfigs(leftConfig, rightConfig) {
  const leftPresent = isObject(leftConfig);
  const rightPresent = isObject(rightConfig);
  if (!leftPresent && !rightPresent) {
    return {
      leftPresent,
      rightPresent,
      equal: true,
      complete: true,
      diffs: [],
      truncated: false,
      skippedDepth: 0,
      compared: 0,
      first: null,
    };
  }
  if (!leftPresent || !rightPresent) {
    const diff = {
      path: "(realized config)",
      kind: "missing",
      left: leftPresent ? leftConfig : undefined,
      right: rightPresent ? rightConfig : undefined,
      missingSide: leftPresent ? "right" : "left",
    };
    return {
      leftPresent,
      rightPresent,
      equal: false,
      complete: true,
      diffs: [diff],
      truncated: false,
      skippedDepth: 0,
      compared: 0,
      first: diff,
    };
  }
  return {
    leftPresent,
    rightPresent,
    ...diffJsonValues(leftConfig, rightConfig),
  };
}
