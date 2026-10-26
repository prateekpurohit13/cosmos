import { el } from "./dom.js";
import { formatCount, formatNs } from "./format.js";
import { finiteTimeNs } from "./trace.js";
import {
  entryClass,
  entryMatches,
  entryOutcome,
  entrySite,
  entryStatus,
  laneOrder,
  traceCounts,
} from "./trace.js";

const ROW_HEIGHT = 24;
const TOP = 30;
const BOTTOM = 10;
const LABEL_WIDTH = 96;
const RIGHT_PAD = 14;
const MINIMAP_HEIGHT = 46;

const CLASS_VARS = {
  Memory: "--blue",
  Network: "--cyan",
  Storage: "--amber",
  Clock: "--violet",
  Process: "--red",
  Random: "--green",
  Unknown: "--faint",
};

const FALLBACK_COLORS = {
  Memory: "#60a5fa",
  Network: "#22d3ee",
  Storage: "#fbbf24",
  Clock: "#a78bfa",
  Process: "#f87171",
  Random: "#4ade80",
  Unknown: "#657080",
};

function cssVar(name, fallback) {
  const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return value || fallback;
}

function colorFor(faultClass, palette) {
  return palette[CLASS_VARS[faultClass] ?? "--faint"] ?? FALLBACK_COLORS[faultClass] ?? palette["--faint"];
}

function setupCanvas(canvas, width, height) {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.max(1, Math.round(width * dpr));
  canvas.height = Math.max(1, Math.round(height * dpr));
  canvas.style.height = `${height}px`;
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return ctx;
}

function nearestIndex(entries, x, left, plotWidth) {
  if (entries.length === 0 || plotWidth <= 0) return -1;
  const clamped = Math.max(left, Math.min(left + plotWidth, x));
  const index = Math.floor(((clamped - left) / plotWidth) * entries.length);
  return Math.min(entries.length - 1, Math.max(0, index));
}

function xFor(index, count, left, plotWidth) {
  return left + ((index + 0.5) / Math.max(1, count)) * plotWidth;
}

function timeTicks(entries, plotWidth) {
  const changes = [];
  let lastAt = null;
  for (let index = 0; index < entries.length; index += 1) {
    const at = finiteTimeNs(entries[index]?.at_ns);
    if (at === null) continue;
    if (lastAt === null || at !== lastAt) {
      changes.push({ index, at });
      lastAt = at;
    }
  }
  if (changes.length <= 1) return changes;
  const target = Math.max(2, Math.min(8, Math.floor(plotWidth / 110)));
  if (changes.length <= target) return changes;
  const picked = [];
  for (let slot = 0; slot < target; slot += 1) {
    const index = Math.round((slot * (changes.length - 1)) / (target - 1));
    const tick = changes[index];
    if (!picked.some((item) => item.index === tick.index)) picked.push(tick);
  }
  return picked;
}

export function createTimeline({ entries, tokens = [], selectedSeq = null, markers = [], onSelect }) {
  const count = entries.length;
  const counts = traceCounts(entries);
  const laneNames = laneOrder(counts);
  if (laneNames.length === 0) laneNames.push("Unknown");
  const failureGroups = (Array.isArray(markers) ? markers : [])
    .filter((marker) => marker && Number.isInteger(marker.index) && marker.index >= 0 && marker.index < count)
    .slice()
    .sort((left, right) => left.index - right.index);
  const failureByIndex = new Map(failureGroups.map((marker) => [marker.index, marker]));

  const tooltip = el("div", { class: "timeline-tooltip", hidden: true });
  const canvas = el("canvas", {
    class: "timeline__canvas",
    tabindex: "0",
    role: "img",
    "aria-label": `Fault timeline with ${count} event${count === 1 ? "" : "s"} and ${failureGroups.length} failed-check marker${failureGroups.length === 1 ? "" : "s"}. Use the left and right arrow keys to select an event.`,
  });
  const minimap = el("canvas", {
    class: "timeline__minimap",
    tabindex: "0",
    role: "img",
    "aria-label": `Trace density minimap with ${failureGroups.length} failed-check marker${failureGroups.length === 1 ? "" : "s"}. Use the left and right arrow keys to select an event.`,
  });
  const frame = el("div", { class: "timeline__frame" }, canvas, tooltip);
  const minimapFrame = el("div", { class: "timeline__minimap-frame" }, minimap);
  const node = el(
    "div",
    { class: "timeline" },
    frame,
    el(
      "p",
      {
        class: "timeline__hint",
      },
      "Event sequence is the horizontal axis. Virtual-time ticks annotate it; wall time is never on this axis." +
        (failureGroups.length > 0
          ? " A dashed red line marks the nearest recorded event at or before a failed check."
          : ""),
    ),
    minimapFrame,
  );

  let selected = selectedSeq;
  let currentTokens = tokens;
  let layout = { width: 0, left: LABEL_WIDTH, plotWidth: 0, height: 0 };
  let frameResize = 0;
  let minimapResize = 0;
  let animationFrame = 0;
  let disposed = false;

  const indexBySeq = new Map();
  entries.forEach((entry, index) => {
    const seq = Number(entry?.seq);
    if (Number.isSafeInteger(seq) && seq >= 0 && !indexBySeq.has(seq)) indexBySeq.set(seq, index);
  });

  const selectedIndex = () => {
    if (selected === null || selected === undefined) return -1;
    return indexBySeq.get(Number(selected)) ?? -1;
  };

  const markSelection = (index) => {
    if (index < 0 || index >= entries.length) {
      selected = null;
      return;
    }
    selected = Number(entries[index]?.seq);
  };

  const draw = () => {
    if (disposed) return;
    const width = Math.round(frame.clientWidth);
    if (width < 80) return;
    const left = LABEL_WIDTH;
    const plotWidth = Math.max(1, width - left - RIGHT_PAD);
    const height = TOP + laneNames.length * ROW_HEIGHT + BOTTOM;
    layout = { width, left, plotWidth, height };
    const ctx = setupCanvas(canvas, width, height);
    const palette = {
      "--text": cssVar("--text", "#e7ecf3"),
      "--text-soft": cssVar("--text-soft", "#c0c8d3"),
      "--muted": cssVar("--muted", "#8b96a5"),
      "--faint": cssVar("--faint", "#657080"),
      "--line": cssVar("--line", "#262c35"),
      "--line-strong": cssVar("--line-strong", "#39424f"),
      "--panel": cssVar("--panel", "#12151a"),
      "--panel-2": cssVar("--panel-2", "#171b21"),
      "--accent": cssVar("--accent", "#7dd3fc"),
      "--blue": cssVar("--blue", "#60a5fa"),
      "--cyan": cssVar("--cyan", "#22d3ee"),
      "--amber": cssVar("--amber", "#fbbf24"),
      "--violet": cssVar("--violet", "#a78bfa"),
      "--red": cssVar("--red", "#f87171"),
      "--green": cssVar("--green", "#4ade80"),
    };

    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = palette["--panel"];
    ctx.fillRect(0, 0, width, height);

    const plotTop = TOP;
    const plotBottom = height - BOTTOM;
    const ticks = timeTicks(entries, plotWidth);

    for (let lane = 0; lane < laneNames.length; lane += 1) {
      const y = plotTop + lane * ROW_HEIGHT;
      ctx.fillStyle = lane % 2 === 0 ? palette["--panel-2"] : palette["--panel"];
      ctx.fillRect(left, y, plotWidth, ROW_HEIGHT);
      ctx.fillStyle = colorFor(laneNames[lane], palette);
      ctx.beginPath();
      ctx.arc(10, y + ROW_HEIGHT / 2, 3, 0, Math.PI * 2);
      ctx.fill();
      ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
      ctx.textAlign = "left";
      ctx.textBaseline = "middle";
      ctx.fillStyle = palette["--text-soft"];
      ctx.fillText(laneNames[lane], 18, y + ROW_HEIGHT / 2);
      const laneCount = counts.classes.get(laneNames[lane]) ?? 0;
      ctx.textAlign = "right";
      ctx.fillStyle = palette["--muted"];
      ctx.fillText(String(laneCount), left - 8, y + ROW_HEIGHT / 2);
    }

    ctx.strokeStyle = palette["--line"];
    ctx.lineWidth = 1;
    for (let lane = 0; lane <= laneNames.length; lane += 1) {
      const y = plotTop + lane * ROW_HEIGHT + 0.5;
      ctx.beginPath();
      ctx.moveTo(left, y);
      ctx.lineTo(left + plotWidth, y);
      ctx.stroke();
    }

    let lastLabelX = -Infinity;
    for (const tick of ticks) {
      const x = xFor(tick.index, count, left, plotWidth);
      ctx.strokeStyle = palette["--line-strong"];
      ctx.globalAlpha = 0.4;
      ctx.beginPath();
      ctx.moveTo(Math.round(x) + 0.5, plotTop - 4);
      ctx.lineTo(Math.round(x) + 0.5, plotBottom);
      ctx.stroke();
      ctx.globalAlpha = 1;
      if (x - lastLabelX < 72) continue;
      lastLabelX = x;
      ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
      ctx.textAlign = x > width - 60 ? "right" : "left";
      ctx.textBaseline = "bottom";
      ctx.fillStyle = palette["--muted"];
      ctx.fillText(`t=${formatNs(tick.at)}`, x, plotTop - 8);
    }

    // Failed checks have no event of their own. Draw them at the nearest recorded decision, and
    // keep the line behind event markers so the selection and injection shapes stay readable.
    if (failureGroups.length > 0) {
      ctx.save();
      ctx.setLineDash([3, 3]);
      ctx.lineWidth = 1;
      ctx.strokeStyle = palette["--red"];
      ctx.globalAlpha = 0.55;
      for (const marker of failureGroups) {
        const x = Math.round(xFor(marker.index, count, left, plotWidth)) + 0.5;
        ctx.beginPath();
        ctx.moveTo(x, plotTop);
        ctx.lineTo(x, plotBottom);
        ctx.stroke();
      }
      ctx.restore();
    }

    const laneY = new Map(laneNames.map((name, index) => [name, plotTop + index * ROW_HEIGHT + ROW_HEIGHT / 2]));
    for (let index = 0; index < entries.length; index += 1) {
      const entry = entries[index];
      const faultClass = entryClass(entry);
      const y = laneY.get(faultClass) ?? plotTop + ROW_HEIGHT / 2;
      const x = xFor(index, count, left, plotWidth);
      const matched = currentTokens.length === 0 || entryMatches(entry, currentTokens);
      const status = entryStatus(entry).toUpperCase();
      const injected = entryOutcome(entry) !== "None";
      ctx.globalAlpha = matched ? 1 : 0.12;
      ctx.fillStyle = colorFor(faultClass, palette);
      ctx.strokeStyle = colorFor(faultClass, palette);
      if (status === "FIRED" && injected) {
        ctx.beginPath();
        ctx.arc(x, y, entries.length > plotWidth ? 1.4 : 2.8, 0, Math.PI * 2);
        ctx.fill();
      } else if (status === "SUPPRESSED") {
        ctx.lineWidth = 1.2;
        ctx.strokeRect(x - 2.4, y - 2.4, 4.8, 4.8);
      } else {
        // A FIRED decision with outcome None, or a skipped decision, is context: draw a tick,
        // not an injection marker.
        ctx.globalAlpha = matched ? 0.62 : 0.08;
        ctx.fillRect(x - 0.5, y - 3, 1.2, 6);
      }
    }
    ctx.globalAlpha = 1;

    if (failureGroups.length > 0) {
      ctx.save();
      ctx.fillStyle = palette["--red"];
      ctx.globalAlpha = 0.9;
      for (const marker of failureGroups) {
        const x = xFor(marker.index, count, left, plotWidth);
        ctx.beginPath();
        ctx.moveTo(x, plotBottom);
        ctx.lineTo(x, plotBottom - 6);
        ctx.lineTo(x + 6, plotBottom);
        ctx.closePath();
        ctx.fill();
      }
      ctx.restore();
    }

    const chosen = selectedIndex();
    if (chosen >= 0) {
      const entry = entries[chosen];
      const faultClass = entryClass(entry);
      const x = xFor(chosen, count, left, plotWidth);
      const y = laneY.get(faultClass) ?? plotTop + ROW_HEIGHT / 2;
      ctx.strokeStyle = palette["--accent"];
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, plotTop);
      ctx.lineTo(x + 0.5, plotBottom);
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(x, y, 5, 0, Math.PI * 2);
      ctx.stroke();
    }

    if (count === 0) {
      ctx.font = "12px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillStyle = palette["--muted"];
      ctx.fillText("No events to draw", left + plotWidth / 2, plotTop + ROW_HEIGHT / 2);
    }

    canvas.setAttribute(
      "aria-label",
      `Fault timeline with ${count} event${count === 1 ? "" : "s"} in ${laneNames.length} lane${laneNames.length === 1 ? "" : "s"} and ${failureGroups.length} failed-check marker${failureGroups.length === 1 ? "" : "s"}. ${currentTokens.length > 0 ? "Non-matching events are dimmed." : ""} Use the left and right arrow keys to select an event.`,
    );
  };

  const drawMinimap = () => {
    if (disposed) return;
    const width = Math.round(minimapFrame.clientWidth);
    if (width < 80) return;
    const height = MINIMAP_HEIGHT;
    const ctx = setupCanvas(minimap, width, height);
    const palette = {
      "--accent": cssVar("--accent", "#7dd3fc"),
      "--line": cssVar("--line", "#262c35"),
      "--panel": cssVar("--panel", "#12151a"),
      "--panel-2": cssVar("--panel-2", "#171b21"),
      "--faint": cssVar("--faint", "#657080"),
      "--blue": cssVar("--blue", "#60a5fa"),
      "--cyan": cssVar("--cyan", "#22d3ee"),
      "--amber": cssVar("--amber", "#fbbf24"),
      "--violet": cssVar("--violet", "#a78bfa"),
      "--red": cssVar("--red", "#f87171"),
      "--green": cssVar("--green", "#4ade80"),
    };
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = palette["--panel-2"];
    ctx.fillRect(0, 0, width, height);

    if (count === 0) return;
    const buckets = Math.max(1, Math.min(count, Math.floor(width / 3)));
    const bucketData = Array.from({ length: buckets }, () => new Map());
    for (let index = 0; index < count; index += 1) {
      const bucket = Math.min(buckets - 1, Math.floor((index / count) * buckets));
      const faultClass = entryClass(entries[index]);
      const row = bucketData[bucket];
      row.set(faultClass, (row.get(faultClass) ?? 0) + 1);
    }
    const totals = bucketData.map((row) => [...row.values()].reduce((sum, value) => sum + value, 0));
    const maxTotal = Math.max(1, ...totals);
    const bucketWidth = width / buckets;
    const available = height - 8;
    for (let bucket = 0; bucket < buckets; bucket += 1) {
      const barWidth = Math.max(1, Math.min(bucketWidth - (buckets > 120 ? 0 : 1), 10));
      const x = bucket * bucketWidth + (bucketWidth - barWidth) / 2;
      let y = height - 4;
      for (const lane of laneNames) {
        const value = bucketData[bucket].get(lane) ?? 0;
        if (value === 0) continue;
        const barHeight = (value / maxTotal) * available;
        ctx.fillStyle = colorFor(lane, palette);
        ctx.fillRect(x, y - barHeight, barWidth, barHeight);
        y -= barHeight;
      }
    }
    if (failureGroups.length > 0) {
      ctx.save();
      ctx.strokeStyle = palette["--red"];
      ctx.globalAlpha = 0.8;
      ctx.lineWidth = 1;
      for (const marker of failureGroups) {
        const x = ((marker.index + 0.5) / Math.max(1, count)) * width;
        ctx.beginPath();
        ctx.moveTo(x + 0.5, 0);
        ctx.lineTo(x + 0.5, height);
        ctx.stroke();
      }
      ctx.restore();
    }
    const chosen = selectedIndex();
    if (chosen >= 0) {
      const x = ((chosen + 0.5) / Math.max(1, count)) * width;
      ctx.strokeStyle = palette["--accent"];
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, height);
      ctx.stroke();
    }
    // Keep a failure marker discoverable when it shares the selection x position: the accent
    // cursor line would otherwise cover the red line completely.
    if (failureGroups.length > 0) {
      ctx.save();
      ctx.fillStyle = palette["--red"];
      for (const marker of failureGroups) {
        const x = ((marker.index + 0.5) / Math.max(1, count)) * width;
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x + 6, 0);
        ctx.lineTo(x, 6);
        ctx.closePath();
        ctx.fill();
      }
      ctx.restore();
    }
    ctx.strokeStyle = palette["--line"];
    ctx.strokeRect(0.5, 0.5, width - 1, height - 1);
  };

  const selectIndex = (index, { notify = true } = {}) => {
    if (index < 0 || index >= entries.length) return;
    markSelection(index);
    draw();
    drawMinimap();
    if (notify && typeof onSelect === "function") onSelect(entries[index], index);
  };

  const nearest = (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    return nearestIndex(entries, x, layout.left, layout.plotWidth);
  };

  const showTooltip = (event) => {
    const index = nearest(event);
    if (index < 0 || entries.length === 0) {
      tooltip.hidden = true;
      return;
    }
    const entry = entries[index];
    const failure = failureByIndex.get(index);
    const failureText = failure
      ? ` · failed check ${failure.failures.map((item) => item.id).join(", ")}`
      : "";
    tooltip.textContent =
      `#${formatCount(entry?.seq)} · t=${formatNs(entry?.at_ns)} · ${entrySite(entry)}` +
      ` · ${entryOutcome(entry)} · ${entryStatus(entry)}${failureText}`;
    const frameRect = frame.getBoundingClientRect();
    const x = event.clientX - frameRect.left;
    const y = event.clientY - frameRect.top;
    tooltip.style.left = `${Math.max(8, x + 12)}px`;
    tooltip.style.top = `${Math.max(8, y - 30)}px`;
    tooltip.style.transform = x > frameRect.width - 240 ? "translateX(-100%)" : "none";
    tooltip.hidden = false;
  };

  const onCanvasClick = (event) => {
    const index = nearest(event);
    if (index >= 0) selectIndex(index);
  };

  const onCanvasMove = (event) => showTooltip(event);
  const onCanvasLeave = () => {
    tooltip.hidden = true;
  };

  const onCanvasKey = (event) => {
    const current = selectedIndex();
    if (event.key === "ArrowLeft") {
      selectIndex(current < 0 ? entries.length - 1 : Math.max(0, current - 1));
      event.preventDefault();
    } else if (event.key === "ArrowRight") {
      selectIndex(current < 0 ? 0 : Math.min(entries.length - 1, current + 1));
      event.preventDefault();
    } else if (event.key === "Home") {
      selectIndex(0);
      event.preventDefault();
    } else if (event.key === "End") {
      selectIndex(entries.length - 1);
      event.preventDefault();
    }
  };

  const onMinimapClick = (event) => {
    if (entries.length === 0) return;
    const rect = minimap.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const index = nearestIndex(entries, x, 0, rect.width);
    if (index >= 0) selectIndex(index);
  };

  const onMinimapKey = (event) => {
    onCanvasKey(event);
  };

  canvas.addEventListener("click", onCanvasClick);
  canvas.addEventListener("mousemove", onCanvasMove);
  canvas.addEventListener("mouseleave", onCanvasLeave);
  canvas.addEventListener("keydown", onCanvasKey);
  minimap.addEventListener("click", onMinimapClick);
  minimap.addEventListener("keydown", onMinimapKey);

  const resizeObserver =
    typeof ResizeObserver === "function"
      ? new ResizeObserver((items) => {
          for (const item of items) {
            if (item.target === frame) {
              const width = Math.round(frame.clientWidth);
              if (width === frameResize) continue;
              frameResize = width;
              draw();
            } else if (item.target === minimapFrame) {
              const width = Math.round(minimapFrame.clientWidth);
              if (width === minimapResize) continue;
              minimapResize = width;
              drawMinimap();
            }
          }
        })
      : null;
  resizeObserver?.observe(frame);
  resizeObserver?.observe(minimapFrame);

  const themeObserver =
    typeof MutationObserver === "function"
      ? new MutationObserver(() => {
          draw();
          drawMinimap();
        })
      : null;
  themeObserver?.observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["data-theme"],
  });

  const redraw = () => {
    frameResize = 0;
    minimapResize = 0;
    draw();
    drawMinimap();
  };

  const setTokens = (nextTokens) => {
    currentTokens = Array.isArray(nextTokens) ? nextTokens : [];
    draw();
  };

  const setSelected = (seq) => {
    selected = seq;
    draw();
    drawMinimap();
  };

  const destroy = () => {
    disposed = true;
    window.cancelAnimationFrame(animationFrame);
    resizeObserver?.disconnect();
    themeObserver?.disconnect();
    canvas.removeEventListener("click", onCanvasClick);
    canvas.removeEventListener("mousemove", onCanvasMove);
    canvas.removeEventListener("mouseleave", onCanvasLeave);
    canvas.removeEventListener("keydown", onCanvasKey);
    minimap.removeEventListener("click", onMinimapClick);
    minimap.removeEventListener("keydown", onMinimapKey);
  };

  // Draw once now and once after layout settles (the first call can see a zero-width frame).
  draw();
  drawMinimap();
  animationFrame = window.requestAnimationFrame(redraw);

  return {
    node,
    laneNames,
    counts,
    selectedIndex,
    selectIndex,
    setTokens,
    setSelected,
    redraw,
    destroy,
  };
}
