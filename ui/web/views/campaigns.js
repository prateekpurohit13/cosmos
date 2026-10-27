import { api } from "../lib/api.js";
import { badge, emptyState, pageHeader, panel, statGrid } from "../lib/components.js";
import { el, frag, link } from "../lib/dom.js";
import { formatCount, formatUnixNs } from "../lib/format.js";

function compareCampaigns(left, right) {
  const l = String(left?.campaign_id ?? "");
  const r = String(right?.campaign_id ?? "");
  return l < r ? -1 : l > r ? 1 : 0;
}

function figure(label, value) {
  return el(
    "div",
    { class: "campaign-card__figure" },
    el("dt", {}, label),
    el("dd", {}, formatCount(value)),
  );
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
  const config = entry.config ?? {};
  return link(
    `/campaigns/${encodeURIComponent(id)}`,
    cardContents(id, entry, config),
    { class: "campaign-card" },
  );
}

function cardContents(id, entry, config) {
  return frag(
    el(
      "div",
      { class: "campaign-card__head" },
      el("span", { class: "campaign-card__name" }, id || "unnamed campaign"),
      entry.complete
        ? badge("complete", "ok")
        : badge("incomplete", "warn", entry.error || "The artifact writer did not finish."),
    ),
    el(
      "dl",
      { class: "campaign-card__figures" },
      figure("Universes", config.trials),
      figure("Nodes", config.node_count),
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
    ),
    entry.error ? el("p", { class: "campaign-card__error" }, entry.error) : null,
  );
}

export async function campaignsView({ signal }) {
  const data = await api.campaigns(signal);
  const campaigns = [...(data.campaigns ?? [])]
    .filter((entry) => entry && typeof entry === "object")
    .sort(compareCampaigns);
  const readable = campaigns.filter(
    (entry) => entry.readable !== false && !entry.catalog_error,
  ).length;
  const unreadable = campaigns.length - readable;

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
        title: "Campaigns",
      }),
      unreadable > 0
        ? statGrid([
            { label: "Campaigns", value: formatCount(campaigns.length), tone: "accent" },
            { label: "Readable", value: formatCount(readable) },
            { label: "Unreadable", value: formatCount(unreadable), tone: "bad" },
          ])
        : null,
      panel({
        title: "Artifact campaigns",
        count: campaigns.length,
        body,
      }),
    ),
  };
}
