function decodeSegment(segment) {
  try {
    return decodeURIComponent(segment);
  } catch {
    return null;
  }
}

export function parseLocation(location = window.location) {
  const raw = location.pathname;
  if (!raw.startsWith("/") || raw.includes("//")) {
    return { name: "not_found", path: raw };
  }
  const path = raw.length > 1 ? raw.replace(/\/+$/, "") : "/";
  if (path === "/" || path === "/campaigns") {
    return { name: "campaigns", path };
  }
  const parts = path.split("/").filter(Boolean);
  if (parts[0] !== "campaigns" || parts.length < 2) {
    return { name: "not_found", path };
  }
  const campaignId = decodeSegment(parts[1]);
  if (campaignId === null) return { name: "not_found", path };

  if (parts.length === 2) {
    return { name: "campaign", campaignId, path };
  }
  if (parts.length === 3 && parts[2] === "compare") {
    return { name: "compare", campaignId, path };
  }
  if (parts.length === 3 && parts[2] === "exploration") {
    return { name: "exploration", campaignId, path };
  }
  if (parts.length === 3 && parts[2] === "universes") {
    return { name: "universes", campaignId, path };
  }
  if (parts.length === 4 && parts[2] === "universes") {
    const seed = decodeSegment(parts[3]);
    if (seed === null) return { name: "not_found", path };
    return { name: "universe", campaignId, seed, path };
  }
  if (parts.length === 5 && parts[2] === "universes" && parts[4] === "timeline") {
    const seed = decodeSegment(parts[3]);
    if (seed === null) return { name: "not_found", path };
    return { name: "timeline", campaignId, seed, path };
  }
  if (parts.length === 5 && parts[2] === "universes" && parts[4] === "state") {
    const seed = decodeSegment(parts[3]);
    if (seed === null) return { name: "not_found", path };
    return { name: "state", campaignId, seed, path };
  }
  if (parts.length === 5 && parts[2] === "universes" && parts[4] === "minimization") {
    const seed = decodeSegment(parts[3]);
    if (seed === null) return { name: "not_found", path };
    return { name: "minimization", campaignId, seed, path };
  }
  if (parts.length === 4 && parts[2] === "findings") {
    const findingId = decodeSegment(parts[3]);
    if (findingId === null) return { name: "not_found", path };
    return { name: "finding", campaignId, findingId, path };
  }
  return { name: "not_found", path };
}

export function navigate(path, { replace = false } = {}) {
  const current = `${window.location.pathname}${window.location.search}`;
  if (path === current) {
    window.dispatchEvent(new Event("cosmos:navigate"));
    return;
  }
  if (replace) {
    window.history.replaceState(null, "", path);
  } else {
    window.history.pushState(null, "", path);
  }
  window.dispatchEvent(new Event("cosmos:navigate"));
}

export function installLinkHandler() {
  document.addEventListener("click", (event) => {
    if (
      event.defaultPrevented ||
      event.button !== 0 ||
      event.metaKey ||
      event.ctrlKey ||
      event.shiftKey ||
      event.altKey
    ) {
      return;
    }
    const anchor = event.target.closest?.("a[href]");
    if (!anchor) return;
    if (anchor.target || anchor.hasAttribute("download") || anchor.dataset.external !== undefined) {
      return;
    }
    const url = new URL(anchor.href, window.location.href);
    if (url.origin !== window.location.origin) return;
    if (!url.pathname.startsWith("/") || url.pathname.startsWith("/api/")) return;
    if (
      url.hash &&
      url.pathname === window.location.pathname &&
      url.search === window.location.search
    ) {
      return;
    }
    event.preventDefault();
    navigate(`${url.pathname}${url.search}`);
  });
}
