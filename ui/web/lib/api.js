export class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.name = "ApiError";
    this.status = Number(status) || 0;
    this.code = code || "request_failed";
  }
}

function campaignPath(campaignId) {
  return `/api/campaigns/${encodeURIComponent(String(campaignId))}`;
}

async function getJson(path, signal) {
  let response;
  try {
    response = await fetch(path, {
      method: "GET",
      headers: { Accept: "application/json" },
      cache: "no-store",
      signal,
    });
  } catch (error) {
    if (error?.name === "AbortError" || signal?.aborted) throw error;
    throw new ApiError(0, "network", "Could not reach the artifact server.");
  }

  let payload = null;
  try {
    payload = await response.json();
  } catch (error) {
    if (error?.name === "AbortError" || signal?.aborted) throw error;
    payload = null;
  }

  if (!response.ok) {
    const code = payload?.error?.code ?? "request_failed";
    const message =
      payload?.error?.message ??
      `The artifact server returned HTTP ${response.status} ${response.statusText}.`;
    throw new ApiError(response.status, code, message);
  }
  if (!payload || payload.schema_version !== 1) {
    throw new ApiError(
      response.status,
      "unsupported_schema",
      "The artifact does not declare schema_version 1.",
    );
  }
  return payload;
}

export const api = {
  campaigns(signal) {
    return getJson("/api/campaigns", signal);
  },

  campaign(campaignId, signal) {
    return getJson(campaignPath(campaignId), signal);
  },

  universe(campaignId, seed, signal) {
    return getJson(
      `${campaignPath(campaignId)}/universes/${encodeURIComponent(String(seed))}`,
      signal,
    );
  },

  trace(campaignId, seed, signal) {
    return getJson(
      `${campaignPath(campaignId)}/universes/${encodeURIComponent(String(seed))}/trace`,
      signal,
    );
  },

  minimization(campaignId, seed, signal) {
    return getJson(
      `${campaignPath(campaignId)}/universes/${encodeURIComponent(String(seed))}/minimization`,
      signal,
    );
  },
};
