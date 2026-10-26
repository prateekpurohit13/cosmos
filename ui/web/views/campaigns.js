import { api } from "../lib/api.js";
import { badge, emptyState, pageHeader, panel, statGrid } from "../lib/components.js";
import { el, frag, link } from "../lib/dom.js";
import { formatCount, formatUnixNs, shortCommit } from "../lib/format.js";
import { setBuildContext } from "../lib/shell.js";

function compareCampaigns(left, right) {
  const l = String(left?.campaign_id ?? "");
  const r = String(right?.campaign_id ?? "");
  return l < r ? -1 : l > r ? 1 : 0;
}

function campaignCard(entry) {
  const id = String(entry.campaign_id ?? "");
  const unreadable = entry.readable === false || Boolean(entry.catalog_error);
  if (unreadable) {
    return el(
      "div",
      { class: "campaign-card campaign-card--error" },
      el(
        "div",
        { class: "campaign-card__head" },
        el("span", { class: "campaign-card__name" }, id || "unnamed campaign"),
        badge("unreadable", "bad"),
      ),
      el(
        "p",
        { class: "campaign-card__error" },
        entry.catalog_error || entry.error || "The artifact could not be read.",
      ),
    );
  }
  const build = entry.build ?? {};
  const config = entry.config ?? {};
  return link(
    `/campaigns/${encodeURIComponent(id)}`,
    cardContents(id, entry, build, config),
    { class: "campaign-card" },
  );
}

function cardContents(id, entry, build, config) {
  return frag(
    el(
      "div",
      { class: "campaign-card__head" },
      el("span", { class: "campaign-card__name" }, entry.name || id),
      entry.complete
        ? badge("complete", "ok")
        : badge("incomplete", "warn", entry.error || "The artifact writer did not finish."),
    ),
    el("div", { class: "campaign-card__id" }, id),
    el(
      "div",
      { class: "campaign-card__meta" },
      el("span", {}, `${formatCount(config.trials)} universes`),
      el("span", {}, `${formatCount(config.node_count)} nodes`),
      el("span", {}, `${formatCount(config.parallel)} workers`),
      config.verify ? el("span", { class: "badge badge--verify" }, "verify") : null,
    ),
    el(
      "div",
      { class: "campaign-card__meta" },
      el("span", {}, `build ${build.id || "unknown"} · ${build.mode || "—"}`),
      el("span", {}, `commit ${shortCommit(build.commit)}`),
    ),
    el(
      "div",
      { class: "campaign-card__meta" },
      el(
        "span",
        {},
        entry.created_unix_ns
          ? `created ${formatUnixNs(entry.created_unix_ns)} wall`
          : "created —",
      ),
      entry.program ? el("span", {}, entry.program) : null,
    ),
    entry.error ? el("p", { class: "campaign-card__error" }, entry.error) : null,
  );
}

export async function campaignsView({ signal }) {
  const data = await api.campaigns(signal);
  setBuildContext();
  const campaigns = [...(data.campaigns ?? [])]
    .filter((entry) => entry && typeof entry === "object")
    .sort(compareCampaigns);
  const readable = campaigns.filter(
    (entry) => entry.readable !== false && !entry.catalog_error,
  ).length;

  const body =
    campaigns.length > 0
      ? el("div", { class: "campaign-list" }, ...campaigns.map(campaignCard))
      : emptyState({
          title: "No campaigns in this artifact root",
          body: "Start the server with an artifact directory, or generate the checked-in demo fixture first.",
          action: el(
            "div",
            { class: "cluster" },
            el("code", { class: "inline-code" }, "nix develop -c just ui-fixture"),
            el("code", { class: "inline-code" }, "nix develop -c just ui"),
          ),
        });

  return {
    title: "Campaigns · Cosmos",
    node: el(
      "div",
      { class: "page" },
      pageHeader({
        eyebrow: "Cosmos / artifact root",
        title: "Campaigns",
        subtitle:
          "Read-only v1 artifacts. Pick a campaign to inspect findings, vacuity, and the universes behind them.",
      }),
      statGrid([
        { label: "Campaigns", value: formatCount(campaigns.length), tone: "accent" },
        { label: "Readable", value: formatCount(readable) },
        { label: "Unreadable", value: formatCount(campaigns.length - readable), tone: campaigns.length - readable ? "bad" : "neutral" },
      ]),
      panel({
        title: "Artifact campaigns",
        subtitle: "Ordered by campaign id; no wall-clock or random ordering.",
        count: campaigns.length,
        body,
      }),
    ),
  };
}
