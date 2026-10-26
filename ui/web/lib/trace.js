// Canonical fault-trace helpers. The server decodes the exact bytes trace_hash() covers
// (include/cosmos/trace.hpp); this module only classifies, searches and displays those frozen
// fields. It never invents node/task/correlation data the trace does not carry.

export const TRACE_CLASS_ORDER = [
  "Memory",
  "Network",
  "Storage",
  "Clock",
  "Process",
  "Random",
  "Unknown",
];

const STATUS_BY_ID = { 0: "FIRED", 1: "SUPPRESSED", 2: "SKIPPED" };
const OUTCOME_BY_ID = {
  0: "None",
  1: "OutOfMemory",
  2: "OpenEio",
  3: "ReadEio",
  4: "WriteEio",
  5: "ShortWrite",
  6: "NoSpace",
  7: "FsyncEio",
  8: "ConnRefused",
  9: "ConnReset",
  10: "PeerClose",
  11: "ShortSend",
  12: "PacketDrop",
  13: "PacketDelay",
  14: "PacketReorder",
  15: "PacketCorrupt",
  16: "ClockStep",
  17: "SleepInterrupted",
  18: "RandomEagain",
  19: "TornWrite",
};

const KEY_ALIASES = new Map([
  ["seq", "seq"],
  ["#", "seq"],
  ["eligible", "eligible"],
  ["eligible_index", "eligible"],
  ["call", "eligible"],
  ["at", "at"],
  ["time", "at"],
  ["t", "at"],
  ["site", "site"],
  ["class", "class"],
  ["fault_class", "class"],
  ["outcome", "outcome"],
  ["kind", "outcome"],
  ["status", "status"],
  ["drew", "drew"],
  ["amount", "amount"],
]);

const NUMERIC_KEYS = new Set(["seq", "eligible", "at", "amount"]);

export function entryStatus(entry) {
  return String(entry?.status ?? STATUS_BY_ID[entry?.status_id] ?? "UNKNOWN");
}

export function entryOutcome(entry) {
  return String(entry?.outcome ?? OUTCOME_BY_ID[entry?.outcome_id] ?? "Unknown");
}

export function entrySite(entry) {
  return String(entry?.site ?? "unknown");
}

export function entryClass(entry) {
  return String(entry?.fault_class ?? "Unknown");
}

// Artifact times are numbers, but a hostile or malformed document can carry null, "" or a
// boolean. Number(null) is 0, so a plain Number() coercion would invent a virtual time at t=0.
// Only a finite number or a non-empty numeric string is a usable time.
export function finiteTimeNs(value) {
  if (typeof value === "number") return Number.isSafeInteger(value) ? value : null;
  if (typeof value === "string") {
    const trimmed = value.trim();
    if (!/^[+-]?\d+$/.test(trimmed)) return null;
    const number = Number(trimmed);
    return Number.isSafeInteger(number) ? number : null;
  }
  return null;
}

export function traceStatusTone(status) {
  switch (String(status ?? "").toUpperCase()) {
    case "FIRED":
      return "warn";
    case "SUPPRESSED":
      return "verify";
    case "SKIPPED":
      return "neutral";
    default:
      return "neutral";
  }
}

// A FIRED decision with outcome None evaluated the gate and injected nothing. Keep the canonical
// status token, but do not paint it as an injection.
export function traceEntryTone(entry) {
  const status = entryStatus(entry).toUpperCase();
  if (status === "FIRED" && entryOutcome(entry) === "None") return "neutral";
  return traceStatusTone(status);
}

export function traceCounts(entries) {
  const counts = {
    total: 0,
    statusFired: 0,
    injected: 0,
    noFault: 0,
    suppressed: 0,
    skipped: 0,
    unknown: 0,
    classes: new Map(),
    outcomes: new Map(),
    minAt: null,
    maxAt: null,
  };
  for (const entry of entries) {
    counts.total += 1;
    const status = entryStatus(entry).toUpperCase();
    const outcome = entryOutcome(entry);
    if (status === "FIRED") {
      counts.statusFired += 1;
      if (outcome === "None") counts.noFault += 1;
      else counts.injected += 1;
    } else if (status === "SUPPRESSED") counts.suppressed += 1;
    else if (status === "SKIPPED") counts.skipped += 1;
    else counts.unknown += 1;

    const faultClass = entryClass(entry);
    counts.classes.set(faultClass, (counts.classes.get(faultClass) ?? 0) + 1);
    counts.outcomes.set(outcome, (counts.outcomes.get(outcome) ?? 0) + 1);

    const at = finiteTimeNs(entry?.at_ns);
    if (at !== null) {
      counts.minAt = counts.minAt === null ? at : Math.min(counts.minAt, at);
      counts.maxAt = counts.maxAt === null ? at : Math.max(counts.maxAt, at);
    }
  }
  return counts;
}

export function laneOrder(counts) {
  const present = [...counts.classes.keys()];
  return present.sort((left, right) => {
    const li = TRACE_CLASS_ORDER.indexOf(left);
    const ri = TRACE_CLASS_ORDER.indexOf(right);
    if (li !== ri) return (li === -1 ? TRACE_CLASS_ORDER.length : li) - (ri === -1 ? TRACE_CLASS_ORDER.length : ri);
    const l = String(left);
    const r = String(right);
    return l < r ? -1 : l > r ? 1 : 0;
  });
}

function tokenize(text) {
  const tokens = [];
  let current = "";
  let quoted = false;
  for (const ch of String(text ?? "")) {
    if (ch === '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && /\s/.test(ch)) {
      if (current) {
        tokens.push(current);
        current = "";
      }
      continue;
    }
    current += ch;
  }
  if (current) tokens.push(current);
  return tokens;
}

// Grammar: whitespace-separated tokens; `key:value` restricts a field, anything else is a
// case-insensitive substring over the frozen fields. A leading `-` negates a token. Quotes keep
// a value with spaces together.
export function parseSearch(text) {
  const parsed = [];
  for (const rawToken of tokenize(text)) {
    let token = rawToken;
    let negated = false;
    while (token.startsWith("-") && token.length > 1) {
      negated = !negated;
      token = token.slice(1);
    }
    if (!token) continue;
    if (/^#\d+$/.test(token)) {
      parsed.push({ key: "seq", value: token.slice(1), negated });
      continue;
    }
    if (token === "drew") {
      parsed.push({ key: "drew", value: "yes", negated });
      continue;
    }
    const colon = token.indexOf(":");
    if (colon > 0 && colon < token.length - 1) {
      const alias = KEY_ALIASES.get(token.slice(0, colon).toLowerCase());
      if (alias) {
        parsed.push({ key: alias, value: token.slice(colon + 1), negated });
        continue;
      }
    }
    parsed.push({ key: null, value: token, negated });
  }
  return parsed;
}

function fieldValues(entry) {
  const status = entryStatus(entry).toLowerCase();
  const drew = entry?.drew ? "yes true 1" : "no false 0";
  return {
    seq: String(entry?.seq ?? ""),
    eligible: String(entry?.eligible_index ?? ""),
    at: String(entry?.at_ns ?? ""),
    site: entrySite(entry).toLowerCase(),
    class: entryClass(entry).toLowerCase(),
    outcome: entryOutcome(entry).toLowerCase(),
    status,
    drew,
    amount: String(entry?.amount_ns ?? ""),
  };
}

function tokenMatches(fields, token) {
  const wanted = token.value.toLowerCase();
  if (token.key === null) {
    const haystack = [
      fields.seq,
      fields.eligible,
      fields.at,
      fields.site,
      fields.class,
      fields.outcome,
      fields.status,
      fields.drew,
      fields.amount,
    ].join(" ");
    return haystack.includes(wanted);
  }
  const actual = fields[token.key];
  if (actual === undefined) return false;
  const numericValue = token.value.replace(/,/g, "");
  if (NUMERIC_KEYS.has(token.key) && /^-?\d+$/.test(numericValue)) {
    return actual === String(Number(numericValue));
  }
  return actual.includes(wanted);
}

export function entryMatches(entry, tokens) {
  if (!tokens || tokens.length === 0) return true;
  const fields = fieldValues(entry);
  for (const token of tokens) {
    const matched = tokenMatches(fields, token);
    if (token.negated ? matched : !matched) return false;
  }
  return true;
}

// Exact correlation only: the canonical trace carries no reason, node or correlation id. A ledger
// row is attached when its site/eligible index/time identify the same decision.
export function findLedgerMatch(ledger, entry) {
  if (!Array.isArray(ledger)) return null;
  const matches = ledger.filter(
    (row) =>
      row &&
      typeof row === "object" &&
      String(row.site) === entrySite(entry) &&
      String(row.eligible_index) === String(entry?.eligible_index) &&
      String(row.at_ns) === String(entry?.at_ns),
  );
  if (matches.length === 0) return null;
  return matches.find((row) => String(row.outcome) === entryOutcome(entry)) ?? matches[0];
}

export function traceSearchHelp() {
  return "site:malloc class:Memory status:FIRED #5 drew:yes amount:50000 -outcome:None";
}

export function selectedSeqFromQuery(params) {
  const raw = params.get("event");
  if (!raw || !/^\d+$/.test(raw)) return null;
  const value = Number(raw);
  return Number.isSafeInteger(value) ? value : null;
}
