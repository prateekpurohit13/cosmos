// Deterministic formatting. No locale APIs: the same artifact must read the same on every host.

function trimFixed(value, digits) {
  return value.toFixed(digits).replace(/\.?0+$/, "");
}

export function formatCount(value) {
  if (value === null || value === undefined) return "—";
  let text;
  if (typeof value === "number") {
    if (!Number.isFinite(value)) return "—";
    text = String(Math.trunc(value));
  } else {
    text = String(value).trim();
  }
  if (!text) return "—";
  if (!/^-?\d+$/.test(text)) return text;
  return text.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}

export function formatNs(value) {
  if (value === null || value === undefined || value === "") return "—";
  const ns = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(ns) || ns < 0) return "—";
  if (ns === 0) return "0ns";
  if (ns < 1_000) return `${Math.trunc(ns)}ns`;
  if (ns < 1_000_000) return `${trimFixed(ns / 1_000, 2)}µs`;
  if (ns < 1_000_000_000) return `${trimFixed(ns / 1_000_000, 2)}ms`;
  return `${trimFixed(ns / 1_000_000_000, 2)}s`;
}

export function formatAt(value) {
  return `t=${formatNs(value)}`;
}

export function formatWallNs(value) {
  const ns = Number(value);
  return Number.isFinite(ns) && ns > 0 ? formatNs(ns) : "—";
}

export function formatUnixNs(value) {
  const ns = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(ns) || ns <= 0) return "—";
  const date = new Date(Math.floor(ns / 1_000_000));
  if (Number.isNaN(date.getTime())) return "—";
  return date.toISOString().replace(".000Z", "Z");
}

export function formatPercent(part, whole) {
  const numerator = Number(part);
  const denominator = Number(whole);
  if (!Number.isFinite(numerator) || !Number.isFinite(denominator) || denominator <= 0) {
    return "—";
  }
  const percent = (numerator / denominator) * 100;
  if (percent > 0 && percent < 0.1) return "<0.1%";
  return `${trimFixed(percent, 1)}%`;
}

export function formatRatio(part, whole) {
  return `${formatCount(part)} / ${formatCount(whole)}`;
}

export function statusTone(status) {
  switch (status) {
    case "passed":
      return "ok";
    case "failed":
    case "crashed":
    case "threw":
      return "bad";
    case "vacuous":
      return "warn";
    case "verify_failed":
      return "verify";
    default:
      return "neutral";
  }
}

export function statusLabel(status) {
  switch (status) {
    case "verify_failed":
      return "verify failed";
    case "vacuous":
      return "vacuous";
    case "threw":
      return "threw";
    case "passed":
      return "passed";
    case "failed":
      return "failed";
    case "crashed":
      return "crashed";
    default:
      return status ? String(status) : "unknown";
  }
}

export function shortId(value, head = 10, tail = 6) {
  const text = value === null || value === undefined ? "" : String(value);
  if (!text) return "—";
  if (text.length <= head + tail + 1) return text;
  return `${text.slice(0, head)}…${text.slice(-tail)}`;
}

export function shortCommit(value) {
  const text = value === null || value === undefined ? "" : String(value);
  if (!text) return "—";
  return text.slice(0, 12);
}

export function findingSeeds(finding) {
  if (!finding || typeof finding !== "object") return [];
  if (Array.isArray(finding.seeds) && finding.seeds.length > 0) {
    const strings = finding.seeds.filter((seed) => typeof seed === "string");
    if (strings.length > 0) return strings;
  }
  if (typeof finding.seed_dec === "string") {
    return [finding.seed_dec];
  }
  // `seed` is a JSON number in the aggregate report for old consumers. It must not be used
  // as a repro identity: values above 2^53 have already lost precision by the time JavaScript
  // parses them. Only the string forms are accepted.
  if (typeof finding.seed === "string") {
    return [finding.seed];
  }
  return [];
}

export function firstFindingSeed(finding) {
  return findingSeeds(finding)[0] ?? "";
}
