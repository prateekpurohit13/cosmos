// Tiny DOM builder. Artifact strings only ever become text nodes or attribute values, never
// innerHTML, so a hostile application detail cannot become markup.

export function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  applyProps(node, props);
  append(node, children);
  return node;
}

function applyProps(node, props) {
  for (const [key, value] of Object.entries(props ?? {})) {
    if (value === undefined || value === null || value === false) continue;
    if (key === "class") {
      node.className = Array.isArray(value) ? value.filter(Boolean).join(" ") : String(value);
    } else if (key === "text") {
      node.textContent = String(value);
    } else if (key === "dataset") {
      for (const [dataKey, dataValue] of Object.entries(value)) {
        if (dataValue !== undefined && dataValue !== null) {
          node.dataset[dataKey] = String(dataValue);
        }
      }
    } else if (key === "attrs") {
      for (const [attrName, attrValue] of Object.entries(value)) {
        if (attrValue !== undefined && attrValue !== null && attrValue !== false) {
          node.setAttribute(attrName, attrValue === true ? "" : String(attrValue));
        }
      }
    } else if (key.startsWith("on") && typeof value === "function") {
      node.addEventListener(key.slice(2).toLowerCase(), value);
    } else if (value === true) {
      node.setAttribute(key, "");
    } else {
      node.setAttribute(key, String(value));
    }
  }
}

export function append(parent, children) {
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    parent.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

export function frag(...children) {
  const fragment = document.createDocumentFragment();
  append(fragment, children);
  return fragment;
}

export function link(href, label, props = {}) {
  return el("a", { href, ...props }, label);
}

export function button(label, onClick, props = {}) {
  const node = el("button", { type: "button", ...props }, label);
  if (typeof onClick === "function") node.addEventListener("click", onClick);
  return node;
}

let toastTimer = 0;

export function toast(message) {
  const node = document.getElementById("toast");
  if (!node) return;
  node.textContent = message;
  node.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    node.hidden = true;
    node.textContent = "";
  }, 1800);
}

export async function copyText(value) {
  const text = value === null || value === undefined ? "" : String(value);
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // Fall through to the selection-based path (also covers a denied clipboard grant).
  }
  const area = document.createElement("textarea");
  area.className = "copy-fallback";
  area.value = text;
  area.setAttribute("readonly", "");
  area.setAttribute("aria-hidden", "true");
  document.body.append(area);
  area.select();
  area.setSelectionRange(0, area.value.length);
  let copied = false;
  try {
    copied = document.execCommand("copy");
  } catch {
    copied = false;
  }
  area.remove();
  return copied;
}

export function copyButton(value, label = "Copy", props = {}) {
  let resetTimer = 0;
  const node = el(
    "button",
    {
      type: "button",
      ...props,
      class: props.class ?? "btn btn--ghost btn--sm",
    },
    label,
  );
  node.addEventListener("click", async () => {
    const resolved = typeof value === "function" ? value() : value;
    const copied = await copyText(resolved);
    node.textContent = copied ? "Copied" : "Copy failed";
    toast(copied ? "Copied to clipboard" : "Could not copy to the clipboard");
    clearTimeout(resetTimer);
    resetTimer = window.setTimeout(() => {
      node.textContent = label;
    }, 1400);
  });
  return node;
}
