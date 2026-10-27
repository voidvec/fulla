// Test-only fake fetch with call recording. No mock libraries: the red
// lines need exact call counts and byte-level body/header inspection.
//
// Dispatch rules:
// - URLs ending in /oauth2/token -> token queue (FIFO); an empty queue
//   synthesizes a valid token response with access_token `fresh-<n>` so
//   forced refreshes (401 retry) always have something to consume.
// - Other URLs -> per-URL route queues; an empty route queue returns 500
//   so un-mocked legs fail loudly instead of hanging.

export interface RecordedCall {
  url: string;
  body: string;
  headers: Record<string, string>;
}

interface QueuedResponse {
  status: number;
  body: string;
}

function headersToRecord(init: RequestInit | undefined): Record<string, string> {
  const out: Record<string, string> = {};
  const source = init?.headers;
  if (!source) return out;
  const entries =
    source instanceof Headers
      ? source.entries()
      : Array.isArray(source)
        ? (source as string[][])
        : Object.entries(source as Record<string, string>);
  for (const [key, value] of entries) out[key.toLowerCase()] = value;
  return out;
}

export function buildAuthTestHarness() {
  const calls: RecordedCall[] = [];
  const tokenQueue: QueuedResponse[] = [];
  const routes = new Map<string, QueuedResponse[]>();
  let autoTokenId = 0;
  const tokenUrls = (): string[] => calls.map((c) => c.url).filter((u) => u.endsWith('/oauth2/token'));

  const pushTokenResponse = (response: QueuedResponse) => {
    tokenQueue.push(response);
  };

  return {
    calls,
    fetch: async (input: string | URL | Request, init?: RequestInit): Promise<Response> => {
      const url = typeof input === 'string' ? input : input instanceof URL ? input.href : input.url;
      const record: RecordedCall = { url, body: typeof init?.body === 'string' ? init.body : '', headers: headersToRecord(init) };
      calls.push(record);
      if (url.endsWith('/oauth2/token')) {
        if (tokenQueue.length > 0) {
          const queued = tokenQueue.shift() as QueuedResponse;
          return new Response(queued.body, { status: queued.status });
        }
        autoTokenId += 1;
        return new Response(
          JSON.stringify({ access_token: `fresh-${autoTokenId}`, token_type: 'Bearer', expires_in: 3600 }),
          { status: 200 },
        );
      }
      const queue = routes.get(url);
      if (queue && queue.length > 0) {
        const queued = queue.shift() as QueuedResponse;
        return new Response(queued.body, { status: queued.status });
      }
      return new Response(JSON.stringify({ error: 'unmocked_url', url }), { status: 500 });
    },

    /** Queue a 200 /oauth2/token response (defaults filled, overrides win). */
    respondWithToken: (overrides: Record<string, unknown> = {}) => {
      autoTokenId += 1;
      const body = { access_token: `fresh-${autoTokenId}`, token_type: 'Bearer', expires_in: 3600, ...overrides };
      pushTokenResponse({ status: 200, body: JSON.stringify(body) });
    },
    respondWithError: (status: number, error: string, errorDescription?: string) => {
      const body: Record<string, string> = { error };
      if (errorDescription) body['error_description'] = errorDescription;
      pushTokenResponse({ status, body: JSON.stringify(body) });
    },
    respondWithRaw: (status: number, text: string) => {
      pushTokenResponse({ status, body: text });
    },

    route: (url: string) => {
      const queue: QueuedResponse[] = [];
      routes.set(url, queue);
      return {
        queueResponses: (...responses: { status: number; body: unknown }[]) => {
          for (const r of responses) queue.push({ status: r.status, body: JSON.stringify(r.body) });
        },
        /** Live view of this route's recorded calls. */
        get calls(): RecordedCall[] {
          return calls.filter((c) => c.url === url);
        },
      };
    },

    tokenEndpointCalls: () => tokenUrls().length,
  };
}
