// Chunk worker: invoked by pg_net right after a chunk is registered, after each committed chunk
// (to pick up the next one), and by the pg_cron sweep when queued messages are visible.
// Only callers holding the project secret key are accepted. Answers at once, works in the
// background (see _shared/background.ts).
//   ai mode:       Deepgram transcription + sentence assembly (ai_pipeline.ts)
//   loopback mode: copy the lecturer's audio to every active language (pipeline.ts)
import { withSupabase } from "npm:@supabase/server@1.9.2";
import { type AiDb, processSttBatch } from "../_shared/ai_pipeline.ts";
import { respondThenRun } from "../_shared/background.ts";
import { resolveMode } from "../_shared/mode.ts";
import { type Db, processBatch } from "../_shared/pipeline.ts";
import type { GeminiApi } from "../_shared/providers.ts";

export default {
  fetch: withSupabase({ auth: "secret" }, (req, ctx) => {
    const { mode, deepgramKey, geminiKey } = resolveMode(Deno.env);
    return respondThenRun(req, mode, () =>
      mode === "ai"
        ? processSttBatch(ctx.supabaseAdmin as unknown as AiDb, {
          deepgramKey,
          geminiKey,
          geminiApi: (Deno.env.get("GEMINI_API") ?? "interactions") as GeminiApi,
        }, { maxJobs: 4, vtSec: 60, deadlineMs: 25_000 })
        : processBatch(ctx.supabaseAdmin as unknown as Db, {
          mode: "loopback",
          maxJobs: 4,
          visibilityTimeoutSec: 60,
          deadlineMs: 20_000,
        }));
  }),
};
