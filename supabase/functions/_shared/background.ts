// Edge Functions are woken by pg_net, which sends its requests in batches and waits for each
// batch to finish (up to its 10 s timeout). A worker that held the request open while a slow
// provider answered therefore delayed the wake-ups for the next chunks (measured 2026-11-12:
// chunks waited 8-21 s to start while Gemini took 16-30 s to fail). Workers now answer at once
// and finish in the background (EdgeRuntime.waitUntil). "?sync=1" keeps the old behaviour for
// manual checks.
declare const EdgeRuntime: { waitUntil(p: Promise<unknown>): void } | undefined;

export async function respondThenRun(req: Request, mode: string, work: () => Promise<unknown>): Promise<Response> {
  const sync = new URL(req.url).searchParams.get("sync") === "1";
  if (!sync && typeof EdgeRuntime !== "undefined" && EdgeRuntime?.waitUntil) {
    const started = Date.now();
    EdgeRuntime.waitUntil(work().then(
      (r) => console.log(JSON.stringify({ mode, elapsedMs: Date.now() - started, result: r })),
      (e) => console.error(`${mode} worker failed: ${(e as Error).message}`),
    ));
    return Response.json({ mode, accepted: true }, { status: 202 });
  }
  const started = Date.now();
  const result = await work();
  return Response.json({ mode, elapsedMs: Date.now() - started, ...(result as object) });
}
