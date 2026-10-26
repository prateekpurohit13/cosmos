import { campaignsView } from "./views/campaigns.js";
import { campaignView } from "./views/campaign.js";
import { universesView } from "./views/universes.js";
import { universeView } from "./views/universe.js";
import { timelineView } from "./views/timeline.js";
import { compareView } from "./views/compare.js";
import { findingView } from "./views/finding.js";
import { errorView } from "./views/error.js";
import { notFoundView } from "./views/not-found.js";
import { el } from "./lib/dom.js";
import { installLinkHandler, parseLocation } from "./lib/router.js";
import { setBuildContext } from "./lib/shell.js";

const THEME_KEY = "cosmos.theme";
const main = document.getElementById("main");
const themeToggle = document.getElementById("theme-toggle");

function applyTheme(theme) {
  const next = theme === "light" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  if (themeToggle) {
    themeToggle.textContent = next === "dark" ? "Light" : "Dark";
    themeToggle.setAttribute(
      "aria-label",
      next === "dark" ? "Switch to light theme" : "Switch to dark theme",
    );
  }
}

function savedTheme() {
  try {
    const stored = window.localStorage.getItem(THEME_KEY);
    if (stored === "light" || stored === "dark") return stored;
  } catch {
    // Private mode or a storage policy can deny localStorage; default to dark.
  }
  return "dark";
}

if (themeToggle) {
  applyTheme(savedTheme());
  themeToggle.addEventListener("click", () => {
    const next = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
    applyTheme(next);
    try {
      window.localStorage.setItem(THEME_KEY, next);
    } catch {
      // Not persisting is harmless.
    }
  });
} else {
  applyTheme("dark");
}

function updateNav(route) {
  const primary = document.querySelector(".topnav a[href='/']");
  if (!primary) return;
  if (route.name === "campaigns") {
    primary.setAttribute("aria-current", "page");
  } else {
    primary.removeAttribute("aria-current");
  }
}

function focusMain() {
  if (main) main.focus({ preventScroll: true });
}

function restoreFocus(samePath, focusId) {
  if (samePath && focusId && focusId !== "main") {
    const target = document.getElementById(focusId);
    if (target instanceof HTMLElement && target.isConnected) {
      target.focus({ preventScroll: true });
      return;
    }
  }
  focusMain();
}

function restorePosition(samePath, focusId, scrollY) {
  // Replacing main with the loading state collapses the page and clamps scrollY to zero. Put the
  // reader back where they were when only the query string changed (compare selection, timeline
  // event links), then restore focus without scrolling again.
  if (samePath) window.scrollTo(0, scrollY);
  restoreFocus(samePath, focusId);
}

function loadingView() {
  return el(
    "div",
    { class: "page" },
    el("p", { class: "loading", role: "status" }, "Loading artifact…"),
  );
}

async function viewFor(route, signal) {
  switch (route.name) {
    case "campaigns":
      return campaignsView({ signal });
    case "campaign":
      return campaignView({ campaignId: route.campaignId, signal });
    case "universes":
      return universesView({ campaignId: route.campaignId, signal });
    case "universe":
      return universeView({ campaignId: route.campaignId, seed: route.seed, signal });
    case "timeline":
      return timelineView({ campaignId: route.campaignId, seed: route.seed, signal });
    case "compare":
      return compareView({ campaignId: route.campaignId, signal });
    case "finding":
      return findingView({
        campaignId: route.campaignId,
        findingId: route.findingId,
        signal,
      });
    default:
      return notFoundView({ path: route.path });
  }
}

let renderToken = 0;
let activeController = null;
let activeView = null;
let lastRenderedPath = null;

function disposeView() {
  if (!activeView) return;
  try {
    activeView.destroy?.();
  } catch {
    // A view teardown must never stop the next route from rendering.
  }
  activeView = null;
}

async function render() {
  const token = ++renderToken;
  const previousPath = lastRenderedPath;
  const focusId = document.activeElement?.id || null;
  const scrollY = window.scrollY;
  if (activeController) activeController.abort();
  activeController = new AbortController();
  const { signal } = activeController;
  const route = parseLocation();
  updateNav(route);

  disposeView();
  main.setAttribute("aria-busy", "true");
  main.replaceChildren(loadingView());
  setBuildContext();
  const samePath = previousPath !== null && previousPath === window.location.pathname;
  if (!samePath) window.scrollTo(0, 0);

  try {
    const view = await viewFor(route, signal);
    if (token !== renderToken || signal.aborted) {
      view.destroy?.();
      return;
    }
    activeView = view;
    main.replaceChildren(view.node);
    document.title = view.title;
    restorePosition(samePath, focusId, scrollY);
  } catch (error) {
    if (token !== renderToken || signal.aborted || error?.name === "AbortError") return;
    activeView = null;
    const view =
      Number(error?.status) === 404
        ? notFoundView({ message: error?.message })
        : errorView(error);
    main.replaceChildren(view.node);
    document.title = view.title;
    restorePosition(samePath, focusId, scrollY);
  } finally {
    if (token === renderToken) {
      main.removeAttribute("aria-busy");
      lastRenderedPath = window.location.pathname;
    }
  }
}

const skipLink = document.querySelector(".skip-link");
if (skipLink) {
  skipLink.addEventListener("click", (event) => {
    if (!main) return;
    event.preventDefault();
    main.focus({ preventScroll: true });
    main.scrollIntoView({ block: "start" });
  });
}

installLinkHandler();
window.addEventListener("popstate", render);
window.addEventListener("cosmos:navigate", render);
window.addEventListener("cosmos:refresh", render);
render();
