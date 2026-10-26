import { api } from "../lib/api.js";
import {
  badge,
  buildBadges,
  buildContext,
  classChip,
  codeBlock,
  copyButton,
  emptyState,
  findingLink,
  kvList,
  notice,
  pageHeader,
  panel,
  seedLink,
  statGrid,
  statusBadge,
  timelineHref,
} from "../lib/components.js";
import { el, link } from "../lib/dom.js";
import {
  formatAt,
  formatCount,
  formatNs,
  formatPercent,
  formatRatio,
} from "../lib/format.js";
import { setBuildContext } from "../lib/shell.js";

function findingLabel(findingId) {
  const text = String(findingId ?? "");
  if (!text) return "—";
  if (text.startsWith("cosmos.")) {
    return el("code", { class: "inline-code" }, text);
  }
  return el("span", {}, text);
}

function keptSeqs(report) {
  const seqs = Array.isArray(report.only_faults) ? report.only_faults : [];
  return new Set(seqs.map((seq) => Number(seq)).filter((seq) => Number.isSafeInteger(seq)));
}

function entryRow(campaignId, seed, entry, kept, refused) {
  const isKept = kept.has(entry.seq);
  const amount = entry.outcome === "None" ? "—" : formatNs(entry.amount_ns);
  return el(
    "tr",
    { class: isKept ? "is-kept" : "is-dim" },
    el(
      "td",
      { class: "num" },
      link(
        timelineHref(campaignId, seed, { eventSeq: entry.seq }),
        `#${formatCount(entry.seq)}`,
        { title: `Open timeline at event ${entry.seq}` },
      ),
    ),
    el("td", { class: "num" }, formatAt(entry.at_ns)),
    el("td", {}, classChip(entry.fault_class)),
    el("td", {}, el("code", { class: "mono" }, entry.site ?? "—")),
    el(
      "td",
      {},
      entry.outcome === "None"
        ? el("span", { class: "faint" }, "None")
        : el("code", { class: "mono" }, entry.outcome),
    ),
    el("td", { class: "num" }, amount),
    el("td", {}, entry.drew ? "yes" : "no"),
    el("td", {}, entry.status ?? "—"),
    el(
      "td",
      {},
      refused
        ? el("span", { class: "faint" }, "—")
        : isKept
          ? badge("kept", "ok")
          : badge("dropped", "plain"),
    ),
  );
}

const PAGE_SIZE = 100;

function tablePage(entries, param) {
  const totalPages = Math.max(1, Math.ceil(entries.length / PAGE_SIZE));
  const raw =
    Number.parseInt(new URLSearchParams(window.location.search).get(param) ?? "1", 10) || 1;
  const page = Math.min(Math.max(1, raw), totalPages);
  return {
    page,
    totalPages,
    entries: entries.slice((page - 1) * PAGE_SIZE, page * PAGE_SIZE),
  };
}

function pageHref(param, page) {
  const params = new URLSearchParams(window.location.search);
  if (page <= 1) params.delete(param);
  else params.set(param, String(page));
  const query = params.toString();
  return `${window.location.pathname}${query ? `?${query}` : ""}`;
}

function tablePager(total, page, totalPages, param, label) {
  if (totalPages <= 1) return null;
  const start = (page - 1) * PAGE_SIZE + 1;
  const end = Math.min(page * PAGE_SIZE, total);
  return el(
    "div",
    { class: "pager" },
    el(
      "span",
      {},
      `${formatCount(start)}–${formatCount(end)} of ${formatCount(total)} ${label} · page ` +
        `${formatCount(page)} of ${formatCount(totalPages)}`,
    ),
    el(
      "div",
      { class: "pager__buttons" },
      page > 1
        ? link(pageHref(param, page - 1), "Previous", { class: "btn btn--ghost btn--sm" })
        : el("span", { class: "btn btn--ghost btn--sm", "aria-disabled": "true" }, "Previous"),
      page < totalPages
        ? link(pageHref(param, page + 1), "Next", { class: "btn btn--ghost btn--sm" })
        : el("span", { class: "btn btn--ghost btn--sm", "aria-disabled": "true" }, "Next"),
    ),
  );
}

function entryTable(
  campaignId,
  seed,
  entries,
  kept,
  { caption, pageParam, label, refused = false },
) {
  if (!entries.length) return null;
  const { page, totalPages, entries: pageEntries } = tablePage(entries, pageParam);
  return el(
    "div",
    {},
    el(
      "div",
      { class: "table-wrap" },
      el(
        "table",
        { class: "data data--minimization" },
        el("caption", { class: "sr-only" }, caption),
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { scope: "col", class: "num" }, "Seq"),
            el("th", { scope: "col", class: "num" }, "Virtual time"),
            el("th", { scope: "col" }, "Class"),
            el("th", { scope: "col" }, "Site"),
            el("th", { scope: "col" }, "Outcome"),
            el("th", { scope: "col", class: "num" }, "Amount"),
            el("th", { scope: "col" }, "Drew"),
            el("th", { scope: "col" }, "Status"),
            el("th", { scope: "col" }, "Set"),
          ),
        ),
        el(
          "tbody",
          {},
          ...pageEntries.map((entry) => entryRow(campaignId, seed, entry, kept, refused)),
        ),
      ),
    ),
    tablePager(entries.length, page, totalPages, pageParam, label),
  );
}

function reportStats(report) {
  const candidates = Array.isArray(report.candidates) ? report.candidates.length : 0;
  const kept = Array.isArray(report.kept) ? report.kept.length : 0;
  return statGrid([
    { label: "Candidates", value: formatCount(candidates), note: "candidate fires in the trace" },
    {
      label: "Kept",
      value: formatCount(kept),
      note: report.refused
        ? "refused report"
        : kept > 0
          ? "1-minimal set"
          : "reproduces with every fault withheld",
      tone: report.refused || kept === 0 ? "neutral" : "warn",
    },
    {
      label: "Kept share",
      value: report.refused ? "—" : formatPercent(kept, candidates),
      note: "of candidate fires",
    },
    { label: "Passes", value: formatCount(report.passes), note: "greedy reduction passes" },
    {
      label: "Replays",
      value: formatCount(report.replays),
      note: "baseline included",
    },
    {
      label: "Crashed trials",
      value: formatCount(report.crashed_trials),
      note: "signal terminations; a no-verdict exit is also not reproduction",
      tone: Number(report.crashed_trials) > 0 ? "bad" : "neutral",
    },
  ]);
}

function minimalNotice(keptCount) {
  const body =
    keptCount === 0
      ? "The kept set is empty: the finding reproduces with every fault withheld. That is still " +
        "a 1-minimal result, but it does not claim an injected fault caused the finding, and it " +
        "does not prove a smaller set is impossible."
      : "Every kept fire is necessary: removing any single one makes the finding stop reproducing. " +
        "That is the 1-minimal guarantee only. This report does not prove the set is the smallest " +
        "possible, and it does not claim a cause outside the recorded trace. A removal trial that " +
        "crashed or exited without a verdict is not counted as reproduction, so necessity is " +
        "unproven for the affected fires.";
  return notice("warn", "1-minimal, not globally minimal", body);
}

function normalizedDetails(report) {
  const target = el("div", { class: "section-gap" });
  const details = el(
    "details",
    { class: "details" },
    el("summary", {}, "Normalized minimization report"),
    target,
  );
  details.addEventListener("toggle", () => {
    if (!details.open || target.dataset.loaded === "1") return;
    target.replaceChildren(codeBlock(JSON.stringify(report, null, 2)));
    target.dataset.loaded = "1";
  });
  return details;
}

function reproPanel(manifest, report, seed) {
  const command = typeof report.repro === "string" ? report.repro : "";
  return panel({
    title: "Exact repro",
    subtitle:
      "The --only-faults set the report kept; the UI does not synthesize or re-run minimization",
    body: el(
      "div",
      {},
      command
        ? el(
            "div",
            { class: "cluster" },
            el("code", { class: "inline-code repro-identity" }, command),
            copyButton(command, "Copy command"),
          )
        : report.refused
          ? emptyState({
              title: "No repro command",
              body: "A refused report does not carry a replayable fault set.",
            })
          : emptyState({
              title: "Empty repro command",
              body:
                "The completed report declares an empty command string. The artifact does not " +
                "carry a runnable invocation to copy.",
            }),
      kvList([
        ["Seed", String(seed)],
        ["Only faults", report.only_faults?.length ? report.only_faults.join(", ") : "none"],
        ["Program", report.program || manifest.program || "—"],
        [
          "Kept hypothesis",
          report.refused
            ? "refused"
            : report.kept?.length
              ? `${formatCount(report.kept.length)} fire${report.kept.length === 1 ? "" : "s"}`
              : "reproduces with every fault withheld",
        ],
      ]),
      notice(
        "info",
        "Report-only",
        "Minimization is exported by the harness. This view never runs it, never changes the " +
          "universe, and never rewrites the canonical trace. The copied command is the harness " +
          "invocation the report declares; the viewer does not verify that the named program " +
          "accepts those flags.",
      ),
    ),
  });
}

export async function minimizationView({ campaignId, seed, signal }) {
  const data = await api.minimization(campaignId, seed, signal);
  const manifest = data.manifest ?? {};
  const universe = data.universe ?? {};
  const report = data.minimization ?? {};
  setBuildContext(buildContext(manifest));

  const seedValue = String(universe.seed ?? seed);
  const present = report.present === true;
  const refused = report.refused === true;
  const kept = keptSeqs(report);
  const candidates = Array.isArray(report.candidates) ? report.candidates : [];
  const keptEntries = Array.isArray(report.kept) ? report.kept : [];
  const firstKept = keptEntries[0];
  const findingId = report.finding_id ? String(report.finding_id) : "";

  const header = pageHeader({
    eyebrow: `Campaign / ${campaignId} / minimization`,
    title: presentingTitle(present, refused),
    subtitle: presentingSubtitle(present, refused, report),
    crumbs: [
      { label: "Campaigns", href: "/" },
      {
        label: manifest.name || campaignId,
        href: `/campaigns/${encodeURIComponent(campaignId)}`,
      },
      {
        label: `Universe ${seedValue}`,
        href: `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(seedValue)}`,
      },
      { label: "Minimization" },
    ],
    badges: [
      seedLink(campaignId, seedValue, { class: "badge badge--plain" }),
      universe.status ? statusBadge(universe.status) : null,
      present && !refused ? badge("1-minimal", "warn", "Not globally minimal") : null,
      refused ? badge("refused", "bad") : null,
      !present ? badge("no report", "plain") : null,
      ...buildBadges(manifest),
    ].filter(Boolean),
    actions: [
      link(
        timelineHref(campaignId, seedValue, {
          eventSeq: firstKept ? firstKept.seq : null,
        }),
        firstKept ? "Timeline at first kept event" : "Open timeline",
        { class: "btn btn--primary" },
      ),
      link(
        `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(seedValue)}`,
        "Universe detail",
        { class: "btn btn--ghost" },
      ),
      findingId
        ? link(
            `/campaigns/${encodeURIComponent(campaignId)}/findings/${encodeURIComponent(findingId)}`,
            "Finding",
            { class: "btn btn--ghost" },
          )
        : null,
    ].filter(Boolean),
  });

  if (!present) {
    return {
      title: `Minimization · Universe ${seedValue} · Cosmos`,
      node: el(
        "div",
        { class: "page" },
        header,
        panel({
          title: "No minimization report",
          subtitle: "Absence is explicit; it is not an empty result and not a success",
          body: el(
            "div",
            {},
            emptyState({
              title: "This seed has not been minimized",
              body:
                "The artifact has no report at " +
                `${report.path || `minimizations/${seedValue}.json`}. Minimization is ` +
                "report-only (O5): the harness exports it separately, and the UI never runs it.",
              action: link(
                `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(seedValue)}`,
                "Inspect the universe",
                { class: "btn btn--ghost" },
              ),
            }),
            notice(
              "info",
              "Not evidence of an empty fault set",
              "A missing report says only that minimization was not exported for this seed. " +
                "It does not mean the finding reproduces without faults.",
            ),
          ),
        }),
      ),
    };
  }

  const body = el(
    "div",
    {},
    refused
      ? notice(
          "bad",
          "Minimization refused",
          report.reason || "The harness refused to minimize this trace.",
        )
      : minimalNotice(keptEntries.length),
    Number(report.crashed_trials) > 0
      ? notice(
          "warn",
          "Crashed replay trials",
          `${formatCount(report.crashed_trials)} of ${formatCount(report.replays)} trials ended in ` +
            "a crash. A crash or exit without a reported check is not counted as reproduction.",
        )
      : null,
    reportStats(report),
    panel({
      title: "Kept fires",
      subtitle: "Exactly the decisions in the --only-faults repro",
      count: keptEntries.length,
      className: "section-gap",
      body: refused
        ? emptyState({
            title: "No kept set",
            body: "A refused report does not claim a minimal fault set.",
          })
        : keptEntries.length > 0
          ? entryTable(campaignId, seedValue, keptEntries, kept, {
              caption: "Kept minimization fires in canonical decision order",
              pageParam: "kept_page",
              label: "kept fires",
            })
          : notice(
              "info",
              "Empty kept set",
              "The finding reproduces with every fault withheld. The report therefore does not " +
                "claim that an injected fault caused it.",
            ),
    }),
    panel({
      title: "Candidates",
      subtitle: refused
        ? "Fault decisions recorded before the refusal; no 1-minimal set was computed"
        : "Candidate fires: decisions that injected or are event sites; kept rows are the 1-minimal set",
      count: candidates.length,
      className: "section-gap",
      body:
        candidates.length === 0
          ? emptyState({
              title: refused ? "No candidates" : "No fault decisions",
              body: refused
                ? "The report was refused before a candidate set was produced."
                : "The recorded trace contains no candidate fault decisions. The kept set is " +
                  "therefore empty; this is a completed report, not a missing one.",
            })
          : entryTable(campaignId, seedValue, candidates, kept, {
              caption: "Minimization candidates in canonical decision order",
              pageParam: "candidates_page",
              label: "candidates",
              refused,
            }),
    }),
    el(
      "div",
      { class: "grid-2 section-gap" },
      reproPanel(manifest, report, seedValue),
      panel({
        title: "Report details",
        subtitle: "Finding identity and the validated fields the server returned",
        body: el(
          "div",
          {},
          kvList([
            ["Finding", findingLabel(findingId)],
            ["Seed", String(seedValue)],
            ["Seed hex", universe.seed_hex || "—"],
            ["Report path", report.path || "—"],
            [
              "Kept / candidates",
              `${formatCount(keptEntries.length)} / ${formatCount(candidates.length)}`,
            ],
            ["Passes", formatCount(report.passes)],
            ["Replays", formatCount(report.replays)],
            ["Crashed trials", formatCount(report.crashed_trials)],
          ]),
          findingId && !findingId.startsWith("cosmos.")
            ? el(
                "p",
                { class: "section-gap" },
                findingLink(campaignId, findingId, "Open the finding"),
                " to inspect prevalence, all failing seeds and the nearest failure evidence.",
              )
            : null,
          normalizedDetails(report),
        ),
      }),
    ),
  );

  return {
    title: `Minimization · Universe ${seedValue} · Cosmos`,
    node: el("div", { class: "page" }, header, body),
  };
}

function presentingTitle(present, refused) {
  if (!present) return "Minimization unavailable";
  if (refused) return "Minimization refused";
  return "Minimization report";
}

function presentingSubtitle(present, refused, report) {
  if (!present) return "No exported MinimizeReport for this seed";
  if (refused) return report.reason || "The harness refused to minimize this trace";
  const kept = Array.isArray(report.kept) ? report.kept.length : 0;
  const candidates = Array.isArray(report.candidates) ? report.candidates.length : 0;
  return `${formatRatio(kept, candidates)} fault decisions kept · 1-minimal, not globally minimal`;
}
