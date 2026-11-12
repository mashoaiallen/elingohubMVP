// Run: npx deno test supabase/functions/_shared/pipeline_test.ts
import { assertEquals } from "jsr:@std/assert@1.0.13";
import {
  type ChunkContext,
  cleanupExpiredAudio,
  type Db,
  type DbError,
  type Job,
  MAX_ATTEMPTS,
  processBatch,
  processJob,
  translatedPath,
} from "./pipeline.ts";

interface Call {
  fn: string;
  args: Record<string, unknown>;
}

/** In-memory stand-in for the database + storage, recording every call. */
function fakeDb(opts: {
  ctx?: ChunkContext | null;
  jobs?: Job[][];
  copyError?: DbError | null;
  copyFailures?: number;
  removeError?: DbError | null;
  candidates?: unknown[];
}) {
  const calls: Call[] = [];
  const copies: { from: string; to: string; bucket?: string }[] = [];
  const removed: { bucket: string; paths: string[] }[] = [];
  const claims = [...(opts.jobs ?? [])];
  let copyFailuresLeft = opts.copyFailures ?? 0;

  const db: Db = {
    rpc(fn, args = {}) {
      calls.push({ fn, args });
      switch (fn) {
        case "worker_claim":
          return Promise.resolve({ data: claims.shift() ?? [], error: null });
        case "worker_begin_chunk":
          return Promise.resolve({ data: opts.ctx === undefined ? null : opts.ctx, error: null });
        case "worker_cleanup_candidates":
          return Promise.resolve({ data: opts.candidates ?? [], error: null });
        default:
          return Promise.resolve({ data: null, error: null });
      }
    },
    storage: {
      from(bucket: string) {
        return {
          copy(from: string, to: string, o?: { destinationBucket?: string }) {
            copies.push({ from, to, bucket: o?.destinationBucket });
            if (copyFailuresLeft > 0) {
              copyFailuresLeft--;
              return Promise.resolve({ data: null, error: { message: "upstream timeout", statusCode: "504" } });
            }
            return Promise.resolve({ data: null, error: opts.copyError ?? null });
          },
          remove(paths: string[]) {
            removed.push({ bucket, paths });
            return Promise.resolve({ data: null, error: opts.removeError ?? null });
          },
        };
      },
    },
  };
  return { db, calls, copies, removed };
}

const chunk = {
  id: 7,
  sessionId: "sess-1",
  hubId: "hub-1",
  seq: 3,
  path: "hub-1/sess-1/000003.wav",
  bytes: 160044,
  durationMs: 5000,
  silent: false,
  status: "processing",
  attempts: 1,
};
const job: Job = { msg_id: 11, read_ct: 1, chunk_id: 7 };
const finishOf = (calls: Call[]) => calls.find((c) => c.fn === "worker_finish_chunk")?.args;

Deno.test("translated path layout", () => {
  assertEquals(translatedPath(chunk, "af"), "hub-1/sess-1/af/000003.wav");
});

Deno.test("no listeners: no copies, no segment rows, chunk skipped", async () => {
  const f = fakeDb({ ctx: { chunk, languages: [], readyLanguages: [] } });
  const r = await processJob(f.db, job, "loopback");
  assertEquals(r.status, "skipped_no_listeners");
  assertEquals(f.copies.length, 0);
  assertEquals(f.calls.filter((c) => c.fn === "worker_segment_ready").length, 0);
  assertEquals(finishOf(f.calls)?.p_status, "skipped_no_listeners");
});

Deno.test("one segment per active language, shared by all its listeners", async () => {
  const f = fakeDb({ ctx: { chunk, languages: ["af", "st"], readyLanguages: [] } });
  const r = await processJob(f.db, job, "loopback");
  assertEquals(r.status, "done");
  assertEquals(f.copies.map((c) => c.to).sort(), ["hub-1/sess-1/af/000003.wav", "hub-1/sess-1/st/000003.wav"]);
  assertEquals(f.copies.every((c) => c.bucket === "translated-audio"), true);
  assertEquals(f.calls.filter((c) => c.fn === "worker_segment_ready").length, 2);
  assertEquals(finishOf(f.calls)?.p_status, "done");
});

Deno.test("redelivery: languages already ready are not regenerated", async () => {
  const f = fakeDb({ ctx: { chunk, languages: ["af", "st"], readyLanguages: ["af"] } });
  await processJob(f.db, job, "loopback");
  assertEquals(f.copies.map((c) => c.to), ["hub-1/sess-1/st/000003.wav"]);
});

Deno.test("redelivery after completion only archives the message", async () => {
  const f = fakeDb({ ctx: { chunk: { ...chunk, status: "done" }, languages: ["af"], readyLanguages: ["af"] } });
  const r = await processJob(f.db, job, "loopback");
  assertEquals(r.status, "already_final");
  assertEquals(f.copies.length, 0);
});

Deno.test("existing destination object counts as success (idempotent copy)", async () => {
  const f = fakeDb({
    ctx: { chunk, languages: ["af"], readyLanguages: [] },
    copyError: { message: "The resource already exists", statusCode: "409" },
  });
  const r = await processJob(f.db, job, "loopback");
  assertEquals(r.status, "done");
});

Deno.test("silent chunk is skipped without any work", async () => {
  const f = fakeDb({ ctx: { chunk: { ...chunk, silent: true }, languages: ["af"], readyLanguages: [] } });
  const r = await processJob(f.db, job, "loopback");
  assertEquals(r.status, "skipped_silent");
  assertEquals(f.copies.length, 0);
});

Deno.test("transient failure leaves the job for retry and logs an error", async () => {
  const f = fakeDb({ ctx: { chunk, languages: ["af"], readyLanguages: [] }, copyFailures: 1 });
  const r = await processJob(f.db, job, "loopback");
  assertEquals(r.status, "retry");
  assertEquals(finishOf(f.calls), undefined); // not archived
  assertEquals(f.calls.some((c) => c.fn === "worker_log" && c.args.p_level === "error"), true);
});

Deno.test("after MAX_ATTEMPTS the job is dead-lettered as failed", async () => {
  const f = fakeDb({ ctx: { chunk, languages: ["af"], readyLanguages: [] }, copyFailures: 1 });
  const r = await processJob(f.db, { ...job, read_ct: MAX_ATTEMPTS }, "loopback");
  assertEquals(r.status, "failed");
  assertEquals(finishOf(f.calls)?.p_status, "failed");
});

Deno.test("batch drains the queue until empty", async () => {
  const f = fakeDb({
    ctx: { chunk, languages: ["af"], readyLanguages: [] },
    jobs: [[job, { ...job, msg_id: 12, chunk_id: 8 }], [{ ...job, msg_id: 13, chunk_id: 9 }]],
  });
  const r = await processBatch(f.db, { mode: "loopback", maxJobs: 4, visibilityTimeoutSec: 60, deadlineMs: 5000 });
  assertEquals(r.processed, 3);
  assertEquals(f.calls.filter((c) => c.fn === "worker_claim").length, 3); // last claim returns empty
});

Deno.test("cleanup removes per bucket then marks rows", async () => {
  const f = fakeDb({
    candidates: [
      { kind: "chunk", row_id: 1, bucket: "source-audio", path: "h/s/000001.wav" },
      { kind: "segment", row_id: 5, bucket: "translated-audio", path: "h/s/af/000001.wav" },
    ],
  });
  const r = await cleanupExpiredAudio(f.db);
  assertEquals(r, { chunks: 1, segments: 1, errors: [] });
  assertEquals(f.removed.length, 2);
  const mark = f.calls.find((c) => c.fn === "worker_mark_deleted")?.args;
  assertEquals(mark, { p_chunk_ids: [1], p_segment_ids: [5] });
});

Deno.test("cleanup does not mark rows whose delete failed", async () => {
  const f = fakeDb({
    candidates: [{ kind: "chunk", row_id: 1, bucket: "source-audio", path: "h/s/000001.wav" }],
    removeError: { message: "boom" },
  });
  const r = await cleanupExpiredAudio(f.db);
  assertEquals(r.chunks, 0);
  assertEquals(f.calls.some((c) => c.fn === "worker_mark_deleted"), false);
});
