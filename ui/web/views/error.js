import { button, el } from "../lib/dom.js";
import { emptyState, pageHeader, panel } from "../lib/components.js";

export function errorView(error) {
  const status = Number(error?.status) || 0;
  const code = error?.code || "request_failed";
  const message =
    error?.message ||
    (status === 0
      ? "Could not reach the artifact server."
      : `The artifact server returned HTTP ${status}.`);

  const body = emptyState({
    title: status === 0 ? "Artifact server unreachable" : "This view could not be loaded",
    body: message,
    action: button(
      "Retry",
      () => window.dispatchEvent(new Event("cosmos:refresh")),
      { class: "btn btn--primary" },
    ),
  });

  const diagnostics = el(
    "p",
    { class: "empty__body" },
    "Request code: ",
    el("code", { class: "inline-code" }, code),
    status ? ` · HTTP ${status}` : "",
  );

  return {
    title: "Could not load · Cosmos",
    node: el(
      "div",
      { class: "page" },
      pageHeader({
        eyebrow: "Artifact error",
        title: "Could not load this view",
        subtitle:
          "The UI reads only the artifact directory given to the server. Check that the campaign directory is complete and schema version 1.",
      }),
      panel({
        title: "Error",
        body: el("div", {}, body, diagnostics),
        tone: "bad",
      }),
    ),
  };
}
