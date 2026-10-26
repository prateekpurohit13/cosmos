// Deterministic replay helpers for the fault timeline. Playback is a client-owned cursor over
// the canonical event sequence: it is paced in events per second and never advances virtual time,
// mutates the universe, or invents an event. A virtual-time target resolves to the nearest
// recorded decision at or before that time, so the cursor always lands on a canonical boundary.

import { entryOutcome, entryStatus, finiteTimeNs } from "./trace.js";

export const PLAYBACK_SPEEDS = [1, 2, 4, 8, 16, 32];
export const DEFAULT_PLAYBACK_SPEED = 4;

// A timer must not schedule below a frame; at the fastest supported speed one event per 31ms is
// already well inside this bound. The bound only protects a future speed change.
const MIN_PLAYBACK_INTERVAL_MS = 16;

export function playbackIntervalMs(speed) {
  const eventsPerSecond = Number(speed);
  if (!Number.isFinite(eventsPerSecond) || eventsPerSecond <= 0) {
    return Math.round(1000 / DEFAULT_PLAYBACK_SPEED);
  }
  return Math.max(MIN_PLAYBACK_INTERVAL_MS, Math.round(1000 / eventsPerSecond));
}

export function isInjected(entry) {
  return entryStatus(entry).toUpperCase() === "FIRED" && entryOutcome(entry) !== "None";
}

export function isSuppressedOrSkipped(entry) {
  const status = entryStatus(entry).toUpperCase();
  return status === "SUPPRESSED" || status === "SKIPPED";
}

// The canonical trace is ordered by event sequence, not necessarily by virtual time (a clock
// fault could step backwards), so scan in event order instead of assuming a sorted array. The
// last entry whose virtual time is at or before the target is the honest boundary. When every
// entry is after the target, the first event is the closest available evidence; the caller labels
// that fallback rather than pretending it is at-or-before.
export function lastIndexAtOrBefore(entries, targetNs) {
  const list = Array.isArray(entries) ? entries : [];
  const target = finiteTimeNs(targetNs);
  if (list.length === 0 || target === null) {
    return { index: -1, atOrBefore: false };
  }
  let found = -1;
  for (let index = 0; index < list.length; index += 1) {
    const at = finiteTimeNs(list[index]?.at_ns);
    if (at !== null && at <= target) found = index;
  }
  if (found >= 0) return { index: found, atOrBefore: true };
  return { index: 0, atOrBefore: false };
}

// Walk forward from `fromIndex`, optionally wrapping once, and return the first matching event.
// This is the only "next" primitive; every playback jump uses it so wrap behaviour is uniform.
export function nextIndex(entries, fromIndex, predicate, { wrap = true, includeFrom = false } = {}) {
  const list = Array.isArray(entries) ? entries : [];
  const count = list.length;
  if (count === 0 || typeof predicate !== "function") return -1;
  const start = Number.isInteger(fromIndex) && fromIndex >= 0 && fromIndex < count ? fromIndex : -1;
  if (start < 0) {
    for (let index = 0; index < count; index += 1) {
      if (predicate(list[index], index)) return index;
    }
    return -1;
  }
  const begin = includeFrom ? 0 : 1;
  const limit = wrap ? count : count - start;
  for (let offset = begin; offset < limit; offset += 1) {
    const index = wrap ? (start + offset) % count : start + offset;
    if (index >= count) break;
    if (predicate(list[index], index)) return index;
  }
  // A full wrapped scan visits every other event but never the start again. If the start is the
  // only match (for example one injected fault that is already selected), report it rather than
  // claiming the trace has none.
  if (wrap && predicate(list[start], start)) return start;
  return -1;
}

const VIRTUAL_TIME_UNITS = new Map([
  ["ns", 1],
  ["us", 1_000],
  ["µs", 1_000],
  ["μs", 1_000],
  ["ms", 1_000_000],
  ["s", 1_000_000_000],
]);

// Accepts the labels the rest of the UI prints: a bare integer (ns), a decimal with a unit, or a
// "t=..." prefix. Commas are allowed as digit grouping. A bare decimal nanoseconds value is
// intentionally accepted too; it is unambiguous as a count of ns. Rejecting anything that is not
// a safe integer keeps virtual time out of the JavaScript rounding zone.
export function parseVirtualTimeNs(text) {
  const raw = String(text ?? "")
    .trim()
    .replace(/,/g, "");
  if (!raw) return null;
  const normalized = raw.replace(/^t\s*=\s*/i, "").trim();
  const match = /^([+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*(ns|us|µs|μs|ms|s)?$/i.exec(normalized);
  if (!match) return null;
  const value = Number(match[1]);
  const factor = VIRTUAL_TIME_UNITS.get((match[2] ?? "ns").toLowerCase());
  if (!Number.isFinite(value) || factor === undefined) return null;
  const ns = Math.round(value * factor);
  return Number.isSafeInteger(ns) ? ns : null;
}

// Failed checks are the only failure evidence the v2 artifact carries for a timeline. Keep the
// artifact order for display, but resolve each check to a canonical event index for a marker.
export function resolveFailedChecks(universe, entries) {
  const checks = Array.isArray(universe?.checks) ? universe.checks : [];
  const list = Array.isArray(entries) ? entries : [];
  let lastAt = null;
  for (const entry of list) {
    const at = finiteTimeNs(entry?.at_ns);
    if (at === null) continue;
    lastAt = lastAt === null ? at : Math.max(lastAt, at);
  }
  const failures = [];
  for (const check of checks) {
    if (!check || check.passed !== false) continue;
    const atNs = finiteTimeNs(check.at_ns);
    const hasTime = atNs !== null;
    let index = -1;
    let atOrBefore = false;
    let relation = "no-events";
    if (hasTime && list.length > 0) {
      const resolved = lastIndexAtOrBefore(list, atNs);
      index = resolved.index;
      atOrBefore = resolved.atOrBefore;
      if (!resolved.atOrBefore) relation = "before-first";
      else if (lastAt !== null && atNs > lastAt) relation = "after-last";
      else relation = "at-or-before";
    } else if (!hasTime) {
      relation = "no-time";
    }
    failures.push({
      id: String(check.id ?? "unknown"),
      detail: typeof check.detail === "string" ? check.detail : "",
      atNs: hasTime ? atNs : null,
      index,
      atOrBefore,
      relation,
    });
  }
  return failures;
}

// Several checks can resolve to the same event (especially at t=0). Group them so the canvas
// draws one marker per x position and the inspector can list every check attached to it.
export function groupFailuresByIndex(failures) {
  const groups = new Map();
  for (const failure of Array.isArray(failures) ? failures : []) {
    if (!failure || !Number.isInteger(failure.index) || failure.index < 0) continue;
    const group = groups.get(failure.index) ?? { index: failure.index, failures: [] };
    group.failures.push(failure);
    groups.set(failure.index, group);
  }
  return [...groups.values()].sort((left, right) => left.index - right.index);
}

export function failureRelationLabel(failure) {
  switch (failure?.relation) {
    case "at-or-before":
      return "nearest at or before";
    case "before-first":
      return "before first recorded event";
    case "after-last":
      return "after last recorded event";
    case "no-time":
      return "virtual time missing";
    case "no-events":
      return "no recorded events";
    default:
      return "unresolved";
  }
}
