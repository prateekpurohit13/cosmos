import { api } from "../lib/api.js";
import {
  badge,
  buildContext,
  emptyState,
  kvList,
  notice,
  pageHeader,
  panel,
  statusBadge,
} from "../lib/components.js";
import { el, link } from "../lib/dom.js";
import { formatAt, formatCount } from "../lib/format.js";
import { setBuildContext } from "../lib/shell.js";

const EVENT_ROW_LIMIT = 500;

function table(headers, rows, { className = "", caption = "" } = {}) {
  if (rows.length === 0) {
    return emptyState({ title: "No rows", body: "This state document has no entries here." });
  }
  return el(
    "div",
    { class: "table-wrap" },
    el(
      "table",
      { class: ["data", className].filter(Boolean).join(" ") },
      caption ? el("caption", { class: "sr-only" }, caption) : null,
      el(
        "thead",
        {},
        el(
          "tr",
          {},
          ...headers.map((header, index) =>
            el(
              "th",
              { scope: "col", class: index === 0 ? undefined : "num" },
              header,
            ),
          ),
        ),
      ),
      el(
        "tbody",
        {},
        ...rows.map((row) => el("tr", {}, ...row.map((cell) => el("td", {}, cell)))),
      ),
    ),
  );
}

function tabs(definitions) {
  const bar = el("div", { class: "tabs", role: "tablist", "aria-label": "Inspection state" });
  const panels = el("div", { class: "tabpanels" });
  let selected = 0;
  const buttons = [];
  const sections = [];

  definitions.forEach((definition, index) => {
    const buttonId = `state-tab-${definition.id}`;
    const panelId = `state-panel-${definition.id}`;
    const button = el(
      "button",
      {
        type: "button",
        role: "tab",
        id: buttonId,
        class: "tab",
        "aria-controls": panelId,
        "aria-selected": index === selected ? "true" : "false",
        tabIndex: index === selected ? 0 : -1,
        onClick: () => select(index),
        onKeyDown: (event) => {
          if (event.key !== "ArrowRight" && event.key !== "ArrowLeft") return;
          event.preventDefault();
          const next =
            event.key === "ArrowRight"
              ? (index + 1) % definitions.length
              : (index - 1 + definitions.length) % definitions.length;
          select(next);
          buttons[next].focus();
        },
      },
      definition.label,
    );
    const section = el(
      "section",
      {
        role: "tabpanel",
        id: panelId,
        class: "tabpanel",
        "aria-labelledby": buttonId,
        hidden: index !== selected,
      },
      definition.body,
    );
    buttons.push(button);
    sections.push(section);
    bar.append(button);
    panels.append(section);
  });

  function select(index) {
    selected = index;
    buttons.forEach((button, buttonIndex) => {
      button.setAttribute("aria-selected", buttonIndex === index ? "true" : "false");
      button.tabIndex = buttonIndex === index ? 0 : -1;
    });
    sections.forEach((section, sectionIndex) => {
      section.hidden = sectionIndex !== index;
    });
  }

  return el("div", { class: "inspector" }, bar, panels);
}

function heapPanel(state) {
  const heap = state?.heap ?? {};
  return panel({
    title: "Heap",
    subtitle: "Active and cumulative allocation counters at the captured point.",
    body: kvList([
      ["Active allocations", formatCount(heap.active_allocations)],
      ["Active bytes", formatCount(heap.active_bytes)],
      ["Total allocated bytes", formatCount(heap.total_allocated_bytes)],
      ["Total allocations", formatCount(heap.total_allocation_count)],
    ]),
  });
}

function schedulerPanel(state) {
  const tasks = Array.isArray(state?.tasks) ? state.tasks : [];
  const mutexes = Array.isArray(state?.mutexes) ? state.mutexes : [];
  const conds = Array.isArray(state?.conds) ? state.conds : [];
  const timers = Array.isArray(state?.timers) ? state.timers : [];
  const deadlines = Array.isArray(state?.deadlines) ? state.deadlines : [];
  return el(
    "div",
    { class: "stack" },
    panel({
      title: "Tasks",
      subtitle: `ready ${formatCount(state?.ready_tasks)} · parked ${formatCount(
        state?.parked_tasks,
      )}${state?.deadlocked ? " · deadlocked" : ""}`,
      body: table(
        ["ID", "Node", "State", "Detached", "Stack", "Joiners"],
        tasks.map((task) => [
          el("span", { class: "mono" }, String(task.id)),
          String(task.node),
          el("span", { class: "mono" }, task.state ?? "?"),
          task.detached ? "yes" : "no",
          formatCount(task.stack_bytes),
          formatCount(task.joiners),
        ]),
        { caption: "Scheduler tasks" },
      ),
    }),
    panel({
      title: "Mutexes and condition variables",
      body: el(
        "div",
        { class: "split" },
        table(
          ["Mutex", "Locked", "Owner", "Waiters"],
          mutexes.map((mutex) => [
            String(mutex.id),
            mutex.locked ? "yes" : "no",
            String(mutex.owner ?? "—"),
            formatCount(mutex.waiters),
          ]),
          { caption: "Mutexes" },
        ),
        table(
          ["Cond", "Waiters"],
          conds.map((cond) => [String(cond.id), formatCount(cond.waiters)]),
          { caption: "Condition variables" },
        ),
      ),
    }),
    panel({
      title: "Timers and deadlines",
      body: el(
        "div",
        { class: "split" },
        table(
          ["Seq", "Task", "Wakeup"],
          timers.map((timer) => [
            String(timer.seq),
            String(timer.task ?? "—"),
            formatAt(timer.wakeup_ns),
          ]),
          { caption: "Task timers" },
        ),
        table(
          ["ID", "At", "Armed"],
          deadlines.map((deadline) => [
            String(deadline.id),
            formatAt(deadline.at_ns),
            deadline.armed ? "yes" : "no",
          ]),
          { caption: "Virtual-time deadlines" },
        ),
      ),
    }),
  );
}

function networkPanel(state) {
  const endpoints = Array.isArray(state?.endpoints) ? state.endpoints : [];
  const partitions = Array.isArray(state?.partitions) ? state.partitions : [];
  return el(
    "div",
    { class: "stack" },
    panel({
      title: "Endpoints",
      subtitle: "Virtual descriptors owned by this universe; no host socket is involved.",
      body: table(
        ["FD", "Node", "Role", "Port", "Peer", "RX bytes", "RX packets", "Pending"],
        endpoints.map((endpoint) => {
          const role = endpoint.listener
            ? "listener"
            : endpoint.awaiting_accept
              ? "awaiting accept"
              : endpoint.connected
                ? "connected"
                : "open";
          return [
            String(endpoint.fd),
            String(endpoint.node),
            role,
            String(endpoint.port),
            endpoint.peer_fd >= 0 ? String(endpoint.peer_fd) : "—",
            formatCount(endpoint.rx_bytes),
            formatCount(endpoint.rx_packets),
            formatCount(endpoint.pending_accepts),
          ];
        }),
        { caption: "Network endpoints" },
      ),
    }),
    panel({
      title: "Partitions",
      body:
        partitions.length === 0
          ? emptyState({ title: "No active partitions", body: "The network is fully connected." })
          : el(
              "ul",
              { class: "plain-list" },
              ...partitions.map((partition) =>
                el(
                  "li",
                  {},
                  el(
                    "span",
                    { class: "mono" },
                    `{${partition.a.join(",")}} | {${partition.b.join(",")}}`,
                  ),
                ),
              ),
            ),
    }),
  );
}

function storagePanel(state) {
  const files = Array.isArray(state?.files) ? state.files : [];
  return panel({
    title: "Files",
    subtitle: "Durability is per file; unsynced bytes are lost at reboot unless tracked.",
    body: table(
      ["ID", "Path", "Node", "Durable", "Unsynced", "Unsynced bytes", "Last sync", "Degraded"],
      files.map((file) => [
        String(file.id),
        el("span", { class: "mono" }, file.path ?? "—"),
        String(file.node),
        formatCount(file.durable_size),
        formatCount(file.unsynced_records),
        formatCount(file.unsynced_bytes),
        file.last_sync_length > 0
          ? `${formatCount(file.last_sync_offset)} + ${formatCount(file.last_sync_length)}`
          : "—",
        file.degraded ? "yes" : "no",
      ]),
      { caption: "Tracked files" },
    ),
  });
}

function nodePanel(state) {
  const nodes = Array.isArray(state?.nodes) ? state.nodes : [];
  return panel({
    title: "Nodes",
    subtitle: "Lifecycle is recorded as node.crash/reboot/pause/resume events.",
    body: table(
      ["ID", "State"],
      nodes.map((node) => [
        String(node.id),
        node.live
          ? el("span", { class: "status status--ok" }, "live")
          : el("span", { class: "status status--bad" }, "crashed"),
      ]),
      { caption: "Simulated nodes" },
    ),
  });
}

function rngPanel(state) {
  const streams = Array.isArray(state?.rng) ? state.rng : [];
  return panel({
    title: "RNG streams",
    subtitle: "Draw counts are observation metadata; replay never reads them.",
    body: table(
      ["Stream", "Seed", "Draws"],
      streams.map((stream) => [
        el("span", { class: "mono" }, stream.stream),
        el("span", { class: "mono" }, String(stream.seed)),
        formatCount(stream.draws),
      ]),
      { caption: "Random streams" },
    ),
  });
}

function telemetryPanel(state) {
  const series = Array.isArray(state?.telemetry) ? state.telemetry : [];
  const labels = Array.isArray(state?.application) ? state.application : [];
  return el(
    "div",
    { class: "stack" },
    panel({
      title: "Application telemetry",
      subtitle: "Names are declarations; events carry only the stable numeric id.",
      body: table(
        ["ID", "Name", "Kind", "Value / total", "Observations"],
        series.map((item) => [
          String(item.id),
          el("span", { class: "mono" }, item.name),
          item.kind,
          formatCount(item.total),
          formatCount(item.observations),
        ]),
        { caption: "Telemetry series" },
      ),
    }),
    panel({
      title: "Application labels",
      subtitle: "Check, coverage and annotation labels used by application.* events.",
      body: table(
        ["ID", "Label"],
        labels.map((label) => [String(label.id), el("span", { class: "mono" }, label.name)]),
        { caption: "Application labels" },
      ),
    }),
  );
}

function eventMatches(entry, query) {
  if (!query) return true;
  const haystack = [
    entry.seq,
    entry.kind,
    entry.domain,
    entry.node,
    entry.task,
    entry.correlation_id,
    entry.payload?.subject,
    entry.payload?.related,
    entry.payload?.value,
    entry.payload?.extra,
    entry.payload?.amount_ns,
  ]
    .filter((value) => value !== null && value !== undefined)
    .join(" ")
    .toLowerCase();
  return haystack.includes(query);
}

function eventsPanel(payload, { setStatus } = {}) {
  const events = payload?.events ?? {};
  const entries = Array.isArray(events.entries) ? events.entries : [];
  const gap = events.gap;
  const warnings = [];
  if (!events.present) {
    warnings.push(
      notice(
        "warn",
        "No inspection stream",
        events.attached
          ? "The artifact says a capture was attached, but no stream is present."
          : "This campaign did not capture the versioned inspection stream for this universe.",
      ),
    );
  }
  if (events.missing) {
    warnings.push(
      notice(
        "bad",
        "Event file is missing",
        "The metadata declares a stream, but the file is not on disk. No event evidence is shown.",
      ),
    );
  }
  if (events.error) {
    warnings.push(
      notice(
        "bad",
        "Event encoder refused the capture",
        `The writer recorded this error instead of an event stream: ${events.error}`,
      ),
    );
  }
  if (gap) {
    warnings.push(
      notice(
        "warn",
        "Capture gap",
        `${formatCount(gap.count)} oldest events were evicted before sequence ${formatCount(
          gap.first_seq,
        )}. The table below is the retained window, not the whole universe.`,
      ),
    );
  }
  if (Number(events.rejected) > 0) {
    warnings.push(
      notice(
        "bad",
        "Rejected producer calls",
        `${formatCount(events.rejected)} record calls were refused before receiving a sequence. ` +
          "This is a producer contract warning, not an eviction.",
      ),
    );
  }
  if (events.present && !events.missing && entries.length === 0 && !gap) {
    warnings.push(
      notice(
        "info",
        "Complete empty capture",
        "A sink was attached and accepted no events. This is not the same as an absent stream.",
      ),
    );
  }

  const domains = [...new Set(entries.map((entry) => entry.domain))].sort();
  const domainSelect = el(
    "select",
    { class: "input input--select", "aria-label": "Filter events by domain" },
    el("option", { value: "" }, "all domains"),
    ...domains.map((domain) => el("option", { value: domain }, domain)),
  );
  const search = el("input", {
    type: "search",
    class: "input",
    placeholder: "search kind, id, node, value…",
    "aria-label": "Search inspection events",
  });
  const status = el("p", { class: "faint", role: "status" });
  const tableHost = el("div", {});

  function renderRows() {
    const query = search.value.trim().toLowerCase();
    const domain = domainSelect.value;
    const filtered = entries.filter(
      (entry) => (!domain || entry.domain === domain) && eventMatches(entry, query),
    );
    const shown = filtered.slice(0, EVENT_ROW_LIMIT);
    tableHost.replaceChildren(
      filtered.length === 0
        ? emptyState({ title: "No matching events", body: "Change the filter or search text." })
        : table(
            ["Seq", "Virtual time", "Kind", "Node", "Task", "Subject", "Value", "Extra", "Amount"],
            shown.map((entry) => [
              el("span", { class: "mono" }, String(entry.seq)),
              formatAt(entry.at_ns),
              el("span", { class: "mono" }, entry.kind),
              entry.node ?? "—",
              entry.task ?? "—",
              entry.payload?.subject ?? "—",
              entry.payload?.value ?? "—",
              entry.payload?.extra ?? "—",
              entry.payload?.amount_ns ?? "—",
            ]),
            { caption: "Inspection events" },
          ),
    );
    const countText = `${formatCount(filtered.length)} of ${formatCount(
      entries.length,
    )} retained events`;
    status.textContent =
      filtered.length > shown.length
        ? `${countText} · showing the first ${formatCount(shown.length)}`
        : countText;
    setStatus?.(filtered.length, entries.length);
  }

  domainSelect.addEventListener("change", renderRows);
  search.addEventListener("input", renderRows);
  renderRows();

  return el(
    "div",
    { class: "stack" },
    ...warnings,
    panel({
      title: "Inspection stream",
      subtitle: events.present
        ? `source ${events.source ?? 3} · version ${events.version ?? 1} · capacity ${formatCount(
            events.capacity,
          )} · retained ${formatCount(events.count)} · size ${formatCount(events.size_bytes)} B`
        : "No stream metadata",
      actions: [domainSelect, search],
      body: el("div", { class: "stack" }, tableHost, status),
    }),
  );
}

export async function stateView({ campaignId, seed, signal }) {
  const universeData = await api.universe(campaignId, seed, signal);
  const manifest = universeData.manifest ?? {};
  const universe = universeData.universe ?? {};
  const state = universe.state ?? null;
  const seedValue = universe.seed ?? seed;
  setBuildContext(buildContext(manifest));

  let eventsPayload = null;
  let eventsError = null;
  try {
    eventsPayload = await api.events(campaignId, seedValue, signal);
  } catch (error) {
    if (error?.name === "AbortError" || signal?.aborted) throw error;
    eventsError = error;
  }

  const views = [];
  const eventCount = Array.isArray(eventsPayload?.events?.entries)
    ? eventsPayload.events.entries.length
    : 0;
  views.push({
    id: "overview",
    label: "Overview",
    body: state
      ? el(
          "div",
          { class: "stack" },
          panel({
            title: "Captured state",
            subtitle: `virtual ${formatAt(state.virtual_time_ns)} · realtime epoch ${
              state.realtime_ns
            } · ${state.deadlocked ? "deadlocked" : "not deadlocked"}`,
            body: kvList([
              ["Seed", el("code", { class: "inline-code" }, String(state.seed))],
              ["Ready tasks", formatCount(state.ready_tasks)],
              ["Parked tasks", formatCount(state.parked_tasks)],
              ["Active allocations", formatCount(state.heap?.active_allocations)],
              ["Active bytes", formatCount(state.heap?.active_bytes)],
            ]),
          }),
          heapPanel(state),
        )
      : notice(
          "warn",
          "No state captured",
          "This artifact predates U3.2 or was exported without state capture. The event stream may still be available.",
        ),
  });
  views.push({ id: "scheduler", label: "Scheduler", body: schedulerPanel(state ?? {}) });
  views.push({ id: "network", label: "Network", body: networkPanel(state ?? {}) });
  views.push({ id: "storage", label: "Storage", body: storagePanel(state ?? {}) });
  views.push({ id: "nodes", label: "Nodes", body: nodePanel(state ?? {}) });
  views.push({ id: "rng", label: "RNG", body: rngPanel(state ?? {}) });
  views.push({ id: "telemetry", label: "Telemetry", body: telemetryPanel(state ?? {}) });
  views.push({
    id: "events",
    label: `Events${eventCount > 0 ? ` (${eventCount})` : ""}`,
    body: eventsError
      ? notice("bad", "Could not load events", eventsError.message || "Unknown error.")
      : eventsPanel(eventsPayload),
  });

  const status = universe.status ?? "unknown";
  const noticeItems = [];
  if (universe.events?.attached && !universe.events?.present) {
    noticeItems.push(
      notice(
        "warn",
        "Inspection stream absent",
        "A sink was attached but no stream was written for this universe.",
      ),
    );
  }

  return {
    title: `State · ${universe.seed_hex ?? seedValue} · Cosmos`,
    node: el(
      "div",
      { class: "page" },
      pageHeader({
        eyebrow: "Replay microscope",
        title: "State inspector",
        subtitle: el(
          "span",
          {},
          "seed ",
          el("code", { class: "inline-code" }, String(universe.seed ?? seedValue)),
          universe.seed_hex ? el("span", { class: "faint" }, ` · ${universe.seed_hex}`) : null,
        ),
        badges: [
          statusBadge(status),
          badge(`universe #${universe.index ?? "—"}`, "neutral"),
          universe.events?.present ? badge("events captured", "ok") : badge("no events", "warn"),
        ],
        actions: [
          link(`/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(
            seedValue,
          )}`, "Universe", { class: "button button--ghost" }),
          link(`/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(
            seedValue,
          )}/timeline`, "Fault timeline", { class: "button button--ghost" }),
        ],
        crumbs: [
          { href: "/", label: "Campaigns" },
          {
            href: `/campaigns/${encodeURIComponent(campaignId)}`,
            label: String(manifest.campaign_id ?? campaignId),
          },
          {
            href: `/campaigns/${encodeURIComponent(campaignId)}/universes/${encodeURIComponent(
              seedValue,
            )}`,
            label: `Universe ${universe.index ?? "—"}`,
          },
          { label: "State" },
        ],
      }),
      ...noticeItems,
      tabs(views),
    ),
  };
}
