export function setBuildContext(...nodes) {
  const host = document.getElementById("build-context");
  if (!host) return;
  host.replaceChildren(...nodes.flat(Infinity).filter((node) => node !== null && node !== undefined));
}
