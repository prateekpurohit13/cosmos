import { button, el, link } from "../lib/dom.js";
import { emptyState, pageHeader, panel } from "../lib/components.js";
import { navigate } from "../lib/router.js";

export function notFoundView({ title = "Not found", message, path } = {}) {
  const where = path ?? (typeof window !== "undefined" ? window.location.pathname : "");
  const body = emptyState({
    title: title || "Not found",
    body:
      message ||
      "No client route or artifact object matches this URL. Campaign and universe links are derived from the loaded artifact.",
    action: el(
      "div",
      { class: "cluster" },
      button(
        "Campaigns",
        () => navigate("/"),
        { class: "btn btn--primary" },
      ),
      link("/", "Open the campaign list", { class: "btn btn--ghost" }),
    ),
  });
  return {
    title: "Not found · Cosmos",
    node: el(
      "div",
      { class: "page" },
      pageHeader({
        eyebrow: "404",
        title: "Not found",
        subtitle: "This URL does not resolve to an artifact view.",
      }),
      panel({
        title: "Route",
        body: el(
          "div",
          {},
          body,
          leftPath(where),
        ),
      }),
    ),
  };
}

function leftPath(path) {
  if (!path) return null;
  return el(
    "p",
    { class: "empty__body" },
    "Requested path: ",
    el("code", { class: "inline-code" }, path),
  );
}
