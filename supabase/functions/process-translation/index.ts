// Translation + TTS worker: one job per (utterance, active language). Invoked by pg_net once per
// new job (languages run in parallel) and by the pg_cron sweep. Only callers holding the project
// secret key are accepted. Answers at once, works in the background (see _shared/background.ts).
import { withSupabase } from "npm:@supabase/server@1.9.2";
import { type AiDb, processTtsBatch } from "../_shared/ai_pipeline.ts";
import { respondThenRun } from "../_shared/background.ts";
import { resolveMode } from "../_shared/mode.ts";
import type { GeminiApi } from "../_shared/providers.ts";

export default {
  fetch: withSupabase({ auth: "secret" }, async (req, ctx) => {
    const { mode, deepgramKey, geminiKey } = resolveMode(Deno.env);
    if (mode !== "ai") return Response.json({ mode, processed: 0, note: "provider keys not configured" });
    // One job per claim keeps each invocation's CPU small; several invocations run in parallel.
    return await respondThenRun(req, mode, () =>
      processTtsBatch(ctx.supabaseAdmin as unknown as AiDb, {
        deepgramKey,
        geminiKey,
        geminiApi: (Deno.env.get("GEMINI_API") ?? "interactions") as GeminiApi,
      }, { maxJobs: 1, vtSec: 90, deadlineMs: 40_000 }));
  }),
};
