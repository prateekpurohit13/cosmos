// The v1 artifact uses stable site/class tokens (include/cosmos/faults.hpp). This table only
// supplies presentation order and grouping; it never invents counters.

const SITE_TABLE = [
  ["malloc", "Memory"],
  ["calloc", "Memory"],
  ["realloc", "Memory"],
  ["open", "Storage"],
  ["read", "Storage"],
  ["write", "Storage"],
  ["fsync", "Storage"],
  ["connect", "Network"],
  ["accept", "Network"],
  ["send", "Network"],
  ["recv", "Network"],
  ["clock_gettime", "Clock"],
  ["nanosleep", "Clock"],
  ["getrandom", "Random"],
  ["crash_node", "Process"],
  ["partition", "Process"],
  ["pause_node", "Process"],
  ["torn_write", "Storage"],
];

const SITE_CLASS = new Map(SITE_TABLE);
const SITE_INDEX = new Map(SITE_TABLE.map(([site], index) => [site, index]));

export function siteClass(site) {
  return SITE_CLASS.get(String(site)) ?? "Unknown";
}

export function siteOrder(site) {
  const index = SITE_INDEX.get(String(site));
  return index === undefined ? Number.MAX_SAFE_INTEGER : index;
}

export function compareSites(left, right) {
  const byOrder = siteOrder(left) - siteOrder(right);
  if (byOrder !== 0) return byOrder;
  const l = String(left);
  const r = String(right);
  return l < r ? -1 : l > r ? 1 : 0;
}

export function siteStatus(activated, eligible, fired, isVacuous = false) {
  if (!activated) {
    return isVacuous
      ? { label: "vacuous", tone: "warn" }
      : { label: "not activated", tone: "neutral" };
  }
  if (!eligible) return { label: "not reached", tone: "warn" };
  if (!fired) return { label: "not fired", tone: "warn" };
  return { label: "fired", tone: "ok" };
}

export function vacuityAdvice(reason) {
  switch (reason) {
    case "never_activated":
      return "Enable the site (or its class) in the configuration.";
    case "never_reached":
      return "Extend warmup or drive the workload far enough to reach this call.";
    case "never_fired":
      return "Raise the rate, or check skip_first and max_injections.";
    default:
      return "Inspect the campaign configuration and workload coverage.";
  }
}
