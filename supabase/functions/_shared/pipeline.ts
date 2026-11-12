// eLingo Hub processing pipeline (shared by Edge Functions).
//
// Delivery model: pgmq gives at-least-once delivery, so every step here is idempotent.
//   * worker_begin_chunk snapshots the languages that have listeners *right now*.
//   * One segment per (chunk, language); repeats are no-ops (unique constraint + "already exists").
//   * A failed job is left on the queue and reappears after the visibility timeout; after
//     MAX_ATTEMPTS it is marked failed and archived (dead letter), with a processing_event row.
//
// "loopback" mode (Milestone 1, and the fallback while provider keys are not set): the source
// chunk is copied (server-side, Storage API) to every active language. That exercises the full
// data path - demand rules, storage, polling, download and playback - with zero AI cost.
// The "ai" mode lives in ai_pipeline.ts.

export type PipelineMode = "loopback" | "ai";

export const MAX_ATTEMPTS = 5;
export const FINAL_STATUSES = ["done", "skipped_silent", "skipped_no_listeners", "failed"] as const;

export interface DbError {
  message: string;
  code?: string;
  statusCode?: string | number;
}

export interface RpcResult<T> {
  data: T | null;
  error: DbError | null;
}

export interface StorageBucketApi {
  copy(
    fromPath: string,
    toPath: string,
    options?: { destinationBucket?: string },
  ): PromiseLike<{ data: unknown; error: DbError | null }>;
  remove(paths: string[]): PromiseLike<{ data: unknown; error: DbError | null }>;
}

/** The subset of a supabase-js client the pipeline uses (lets tests pass a fake). */
export interface Db {
  rpc(fn: string, args?: Record<string, unknown>): PromiseLike<RpcResult<unknown>>;
  storage: { from(bucket: string): StorageBucketApi };
}

export interface Job {
  msg_id: number;
  read_ct: number;
  chunk_id: number;
}

export interface ChunkInfo {
  id: number;
  sessionId: string;
  hubId: string;
  seq: number;
  path: string;
  bytes: number;
  durationMs: number;
  silent: boolean;
  status: string;
  attempts: number;
}

export interface ChunkContext {
  chunk: ChunkInfo;
  languages: string[];
  readyLanguages: string[];
}

export interface JobResult {
  chunkId: number;
  status: "done" | "skipped_silent" | "skipped_no_listeners" | "failed" | "retry" | "already_final";
  languages?: string[];
  error?: string;
}

export interface BatchOptions {
  mode: PipelineMode;
  maxJobs: number;
  visibilityTimeoutSec: number;
  deadlineMs: number;
}

function errorMessage(e: unknown): string {
  if (e instanceof Error) return e.message;
  if (typeof e === "object" && e && "message" in e) return String((e as { message: unknown }).message);
  return String(e);
}

async function call<T>(db: Db, fn: string, args: Record<string, unknown>): Promise<T> {
  const { data, error } = await db.rpc(fn, args);
  if (error) throw new Error(`${fn}: ${error.message}`);
  return data as T;
}

export function translatedPath(chunk: Pick<ChunkInfo, "hubId" | "sessionId" | "seq">, lang: string): string {
  return `${chunk.hubId}/${chunk.sessionId}/${lang}/${String(chunk.seq).padStart(6, "0")}.wav`;
}

function isAlreadyExists(error: DbError): boolean {
  return String(error.statusCode) === "409" || /already exists/i.test(error.message);
}

async function finish(db: Db, job: Job, status: string, error?: string): Promise<void> {
  await call(db, "worker_finish_chunk", {
    p_chunk_id: job.chunk_id,
    p_msg_id: job.msg_id,
    p_status: status,
    p_error: error ?? null,
  });
}

async function loopbackSegment(db: Db, chunk: ChunkInfo, lang: string): Promise<void> {
  const dest = translatedPath(chunk, lang);
  // Server-side copy: no audio bytes pass through this function (keeps CPU and memory low).
  const { error } = await db.storage.from("source-audio").copy(chunk.path, dest, {
    destinationBucket: "translated-audio",
  });
  if (error && !isAlreadyExists(error)) {
    throw new Error(`copy for ${lang}: ${error.message}`);
  }
  await call(db, "worker_segment_ready", {
    p_chunk_id: chunk.id,
    p_language: lang,
    p_path: dest,
    p_bytes: chunk.bytes,
    p_duration_ms: chunk.durationMs,
    p_mode: "loopback",
  });
  await call(db, "worker_record_usage", {
    p_chunk_id: chunk.id,
    p_language: lang,
    p_provider: "loopback",
    p_operation: "storage_copy",
    p_model: "",
    p_units: chunk.durationMs / 1000,
    p_unit_type: "audio_seconds",
  });
}

export async function processJob(db: Db, job: Job, mode: PipelineMode): Promise<JobResult> {
  if (job.chunk_id === null || job.chunk_id === undefined) {
    // Not a chunk (e.g. the AI stage's end-of-session flush message): nothing to do in loopback.
    await db.rpc("worker_queue_archive", { p_queue: "chunk_jobs", p_msg_id: job.msg_id });
    return { chunkId: 0, status: "already_final" };
  }
  try {
    const ctx = await call<ChunkContext | null>(db, "worker_begin_chunk", { p_chunk_id: job.chunk_id });
    if (!ctx) {
      await finish(db, job, "failed", "chunk row not found");
      return { chunkId: job.chunk_id, status: "failed", error: "chunk row not found" };
    }
    const chunk = ctx.chunk;

    // Redelivery of a job that already completed: just archive the message.
    if ((FINAL_STATUSES as readonly string[]).includes(chunk.status)) {
      await finish(db, job, chunk.status);
      return { chunkId: chunk.id, status: "already_final" };
    }
    if (chunk.silent) {
      await finish(db, job, "skipped_silent");
      return { chunkId: chunk.id, status: "skipped_silent" };
    }
    // Demand-driven: nobody is listening, so nothing is generated (and no AI is called).
    if (ctx.languages.length === 0) {
      await finish(db, job, "skipped_no_listeners");
      return { chunkId: chunk.id, status: "skipped_no_listeners", languages: [] };
    }

    const pending = ctx.languages.filter((lang) => !ctx.readyLanguages.includes(lang));
    if (mode === "loopback") {
      await Promise.all(pending.map((lang) => loopbackSegment(db, chunk, lang)));
    } else {
      throw new Error("PIPELINE_MODE=ai is handled by ai_pipeline.ts, not processJob");
    }

    await finish(db, job, "done");
    return { chunkId: chunk.id, status: "done", languages: ctx.languages };
  } catch (e) {
    const message = errorMessage(e);
    await db.rpc("worker_log", {
      p_level: "error",
      p_stage: "process-chunk",
      p_message: message,
      p_chunk_id: job.chunk_id,
      p_detail: { attempt: job.read_ct, msgId: job.msg_id },
    });
    if (job.read_ct >= MAX_ATTEMPTS) {
      await finish(db, job, "failed", message).catch(() => undefined);
      return { chunkId: job.chunk_id, status: "failed", error: message };
    }
    // Leave the message on the queue; it becomes visible again after the visibility timeout.
    return { chunkId: job.chunk_id, status: "retry", error: message };
  }
}

/** Drains the queue until it is empty or the deadline passes (stays well inside function limits). */
export async function processBatch(db: Db, opts: BatchOptions): Promise<{ processed: number; results: JobResult[] }> {
  const started = Date.now();
  const results: JobResult[] = [];
  while (Date.now() - started < opts.deadlineMs) {
    const jobs = await call<Job[] | null>(db, "worker_claim", {
      p_qty: opts.maxJobs,
      p_vt: opts.visibilityTimeoutSec,
    });
    if (!jobs || jobs.length === 0) break;
    results.push(...(await Promise.all(jobs.map((job) => processJob(db, job, opts.mode)))));
  }
  return { processed: results.length, results };
}

export interface CleanupCandidate {
  kind: "chunk" | "segment";
  row_id: number;
  bucket: string;
  path: string;
}

/** Retention: deletes expired audio through the Storage API, then marks the rows. */
export async function cleanupExpiredAudio(db: Db, limit = 200): Promise<{ chunks: number; segments: number; errors: string[] }> {
  const candidates = (await call<CleanupCandidate[] | null>(db, "worker_cleanup_candidates", { p_limit: limit })) ?? [];
  const deletedChunks: number[] = [];
  const deletedSegments: number[] = [];
  const errors: string[] = [];

  const byBucket = new Map<string, CleanupCandidate[]>();
  for (const c of candidates) {
    const list = byBucket.get(c.bucket) ?? [];
    list.push(c);
    byBucket.set(c.bucket, list);
  }
  for (const [bucket, items] of byBucket) {
    for (let i = 0; i < items.length; i += 100) {
      const slice = items.slice(i, i + 100);
      // Removing an object that no longer exists is not an error, so retries are safe.
      const { error } = await db.storage.from(bucket).remove(slice.map((c) => c.path));
      if (error) {
        errors.push(`${bucket}: ${error.message}`);
        continue;
      }
      for (const c of slice) (c.kind === "chunk" ? deletedChunks : deletedSegments).push(c.row_id);
    }
  }
  if (deletedChunks.length || deletedSegments.length) {
    await call(db, "worker_mark_deleted", { p_chunk_ids: deletedChunks, p_segment_ids: deletedSegments });
  }
  return { chunks: deletedChunks.length, segments: deletedSegments.length, errors };
}
