import { copyButton, el, link } from "./dom.js";
import {
  formatCount,
  statusLabel,
  statusTone,
} from "./format.js";

export { copyButton };

export function badge(label, tone = "neutral", title) {
  return el(
    "span",
    {
      class: `badge badge--${tone}`,
      ...(title ? { title } : {}),
    },
    label,
  );
}

export function statusBadge(status, label = statusLabel(status)) {
  return el("span", { class: `status status--${statusTone(status)}` }, label);
}

export function classChip(faultClass) {
  const text = faultClass ? String(faultClass) : "Unknown";
  return el(
    "span",
    { class: "class-chip", dataset: { class: text } },
    el("span", { class: "dot", "aria-hidden": "true" }),
    text,
  );
}

export function breadcrumbs(items) {
  const parts = [];
  items.forEach((item, index) => {
    if (index > 0) {
      parts.push(el("span", { class: "crumbs__sep", "aria-hidden": "true" }, "/"));
    }
    if (item.href) {
      parts.push(link(item.href, item.label, { class: "crumbs__link" }));
    } else {
      parts.push(
        el("span", { class: "crumbs__current", "aria-current": "page" }, item.label),
      );
    }
  });
  return el("nav", { class: "crumbs", "aria-label": "Breadcrumb" }, ...parts);
}

export function pageHeader({
  eyebrow,
  title,
  subtitle,
  badges = [],
  actions = [],
  crumbs = [],
}) {
  return el(
    "header",
    { class: "page-head" },
    crumbs.length > 0 ? breadcrumbs(crumbs) : null,
    eyebrow ? el("p", { class: "eyebrow" }, eyebrow) : null,
    el(
      "div",
      { class: "page-head__row" },
      el(
        "div",
        { class: "page-head__title" },
        el("h1", { class: "page-title" }, title),
        subtitle ? el("p", { class: "lede" }, subtitle) : null,
        badges.length > 0 ? el("div", { class: "badge-row" }, ...badges) : null,
      ),
      actions.length > 0 ? el("div", { class: "page-head__actions" }, ...actions) : null,
    ),
  );
}

export function statGrid(items) {
  return el(
    "dl",
    { class: "stats" },
    ...items.map((item) =>
      el(
        "div",
        { class: `stat stat--${item.tone ?? "neutral"}` },
        el("dt", {}, item.label),
        el("dd", {}, item.value),
        item.note ? el("p", { class: "stat__note" }, item.note) : null,
      ),
    ),
  );
}

export function panel({
  title,
  subtitle,
  count,
  actions = [],
  body,
  tone,
  flush = false,
  id,
  className,
}) {
  return el(
    "section",
    {
      class: ["panel", tone ? `panel--${tone}` : "", className ?? ""]
        .filter(Boolean)
        .join(" "),
      ...(id ? { id } : {}),
    },
    el(
      "header",
      { class: "panel__head" },
      el(
        "div",
        { class: "panel__heading" },
        el("h2", { class: "panel__title" }, title),
        subtitle ? el("p", { class: "panel__subtitle" }, subtitle) : null,
      ),
      el(
        "div",
        { class: "panel__meta" },
        count !== undefined && count !== null ? el("span", { class: "count" }, formatCount(count)) : null,
        ...actions,
      ),
    ),
    el("div", { class: `panel__body${flush ? " panel__body--flush" : ""}` }, body),
  );
}

export function emptyState({ title, body, action }) {
  return el(
    "div",
    { class: "empty" },
    el("p", { class: "empty__title" }, title),
    body ? el("p", { class: "empty__body" }, body) : null,
    action ? el("div", { class: "empty__action" }, action) : null,
  );
}

export function notice(tone, title, body) {
  return el(
    "div",
    { class: `notice notice--${tone}`, role: "note" },
    title ? el("strong", { class: "notice__title" }, title) : null,
    body ? el("span", {}, body) : null,
  );
}

export function meter(value, max, tone = "", label = "prevalence") {
  const ceiling = Number(max) > 0 ? Number(max) : 1;
  const current = Number.isFinite(Number(value)) ? Math.min(Math.max(Number(value), 0), ceiling) : 0;
  return el("progress", {
    class: `meter${tone ? ` meter--${tone}` : ""}`,
    max: ceiling,
    value: current,
    "aria-label": label,
  });
}

export function seedLink(campaignId, seed, props = {}) {
  const text = String(seed);
  const classes = ["seed", props.class].filter(Boolean).join(" ");
  const { class: _ignored, ...rest } = props;
  return link(
    `/campaigns/${encodeURIComponent(String(campaignId))}/universes/${encodeURIComponent(text)}`,
    text,
    { ...rest, class: classes, title: props.title ?? `Universe seed ${text}` },
  );
}

export function timelineHref(campaignId, seed, { checkId = null, atNs = null, eventSeq = null } = {}) {
  const params = new URLSearchParams();
  if (eventSeq !== null && eventSeq !== undefined && String(eventSeq) !== "") {
    const seq = Number(eventSeq);
    if (Number.isSafeInteger(seq) && seq >= 0) params.set("event", String(seq));
  }
  if (checkId !== null && checkId !== undefined && String(checkId) !== "") {
    params.set("check", String(checkId));
  }
  const rawAt = typeof atNs === "number" ? atNs : typeof atNs === "string" ? atNs.trim() : null;
  if (rawAt !== null && (typeof rawAt === "number" || /^[+-]?\d+$/.test(rawAt))) {
    const value = Number(rawAt);
    if (Number.isSafeInteger(value)) params.set("at", String(value));
  }
  const query = params.toString();
  return (
    `/campaigns/${encodeURIComponent(String(campaignId))}/universes/` +
    `${encodeURIComponent(String(seed))}/timeline${query ? `?${query}` : ""}`
  );
}

export function minimizationHref(campaignId, seed) {
  return (
    `/campaigns/${encodeURIComponent(String(campaignId))}/universes/` +
    `${encodeURIComponent(String(seed))}/minimization`
  );
}

export function compareHref(campaignId, leftSeed, rightSeed) {
  const params = new URLSearchParams();
  if (leftSeed !== null && leftSeed !== undefined && String(leftSeed) !== "") {
    params.set("left", String(leftSeed));
  }
  if (rightSeed !== null && rightSeed !== undefined && String(rightSeed) !== "") {
    params.set("right", String(rightSeed));
  }
  const query = params.toString();
  return (
    `/campaigns/${encodeURIComponent(String(campaignId))}/compare` +
    (query ? `?${query}` : "")
  );
}

export function findingLink(campaignId, findingId, label = findingId, props = {}) {
  return link(
    `/campaigns/${encodeURIComponent(String(campaignId))}/findings/${encodeURIComponent(String(findingId))}`,
    label,
    props,
  );
}

export function reproTemplate(manifest) {
  const template = manifest?.repro ?? manifest?.config?.repro;
  if (typeof template !== "string") return null;
  return template.includes("{seed}") ? template : null;
}

export function reproInvocation(manifest, seed) {
  const template = reproTemplate(manifest);
  if (!template || seed === null || seed === undefined || seed === "") return null;
  const build = manifest?.build ?? {};
  const values = {
    seed: String(seed),
    program: manifest?.program || manifest?.config?.program || "unknown",
    campaign: manifest?.campaign_id || "",
    build: build.id || "",
    commit: build.commit || "",
    binary_hash: build.binary_hash || "",
  };
  const output = template.replace(
    /\{(seed|program|campaign|build|commit|binary_hash)\}/g,
    (_, key) => values[key],
  );
  if (/\{[a-zA-Z_]+\}/.test(output)) return null;
  return output;
}

export function reproIdentity(manifest, seed) {
  const build = manifest?.build ?? {};
  const parts = [
    `program=${manifest?.program || manifest?.config?.program || "unknown"}`,
    `campaign=${manifest?.campaign_id || "unknown"}`,
    `seed=${seed ?? "—"}`,
    `build=${build.id || "unknown"}`,
  ];
  if (build.commit) parts.push(`commit=${build.commit}`);
  if (build.binary_hash) parts.push(`binary_hash=${build.binary_hash}`);
  return parts.join(" ");
}

export function buildBadges(manifest) {
  const config = manifest?.config ?? {};
  const badges = [];
  if (manifest?.complete === false) {
    badges.push(badge("incomplete", "warn", "The artifact writer did not finish."));
  }
  if (manifest?.error) badges.push(badge("write error", "bad", manifest.error));
  if (config.verify) badges.push(badge("verify", "verify", "Double-run verification was enabled."));
  if (config.trace) {
    badges.push(
      badge("trace", "info", `Canonical traces kept, capacity ${formatCount(config.trace_capacity)}.`),
    );
  }
  return badges;
}

export function kvList(items, { className = "" } = {}) {
  return el(
    "dl",
    { class: ["kv", className].filter(Boolean).join(" ") },
    ...items.map(([label, value]) => [el("dt", {}, label), el("dd", {}, value)]),
  );
}

export function codeBlock(text, { wrap = false } = {}) {
  return el("pre", { class: `code-block${wrap ? " code-block--wrap" : ""}` }, text);
}

export function detailsBlock(title, body, open = false) {
  return el(
    "details",
    { class: "details", ...(open ? { open: true } : {}) },
    el("summary", {}, title),
    body,
  );
}
