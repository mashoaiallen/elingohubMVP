// Queue worker: invoked by pg_net right after a chunk is registered, and by the pg_cron sweep
// when queued messages are visible. Only callers holding the project secret key are accepted.
import { withSupabase } from "npm:@supabase/server@1.9.2";
import { type Db, type PipelineMode, processBatch } from "../_shared/pipeline.ts";

const MODE = (Deno.env.get("PIPELINE_MODE") ?? "loopback") as PipelineMode;

export default {
  fetch: withSupabase({ auth: "secret" }, async (_req, ctx) => {
    const started = Date.now();
    const result = await processBatch(ctx.supabaseAdmin as unknown as Db, {
      mode: MODE,
      maxJobs: 4,
      visibilityTimeoutSec: 60,
      deadlineMs: 20_000,
    });
    return Response.json({ mode: MODE, elapsedMs: Date.now() - started, ...result });
  }),
};
