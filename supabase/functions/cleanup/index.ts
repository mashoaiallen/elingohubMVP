// Retention job: deletes expired source and translated audio through the Storage API
// (rows in storage.objects must not be deleted directly). Invoked by pg_cron every 5 minutes,
// only when there is something to delete.
import { withSupabase } from "npm:@supabase/server@1.9.2";
import { cleanupExpiredAudio, type Db } from "../_shared/pipeline.ts";

export default {
  fetch: withSupabase({ auth: "secret" }, async (_req, ctx) => {
    const result = await cleanupExpiredAudio(ctx.supabaseAdmin as unknown as Db, 200);
    return Response.json(result);
  }),
};
