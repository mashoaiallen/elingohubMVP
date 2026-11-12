import { assert, assertEquals } from "jsr:@std/assert@1.0.13";
import {
  emptyState,
  GUARD_SEC,
  type PlacedWord,
  PREROLL_SEC,
  segment,
  skipChunk,
  stitch,
  type SttState,
  type Word,
} from "./assembler.ts";

// A simulated lecture: words with absolute times; each chunk "transcribes" exactly the words
// audible in its window (pre-roll included), like a perfect recogniser would.
interface AbsWord {
  text: string;
  t0: number;
  t1: number;
}

function lecture(text: string, startAt = 0.2, wordSec = 0.38, gapSec = 0.12): AbsWord[] {
  let t = startAt;
  return text.split(" ").map((w) => {
    const word = { text: w, t0: t, t1: t + wordSec };
    t += wordSec + gapSec;
    return word;
  });
}

/** Words heard in [from, to): a word cut by the window edge is heard only if mostly inside. */
function hear(words: AbsWord[], from: number, to: number): Word[] {
  return words
    .filter((w) => {
      const inside = Math.min(w.t1, to) - Math.max(w.t0, from);
      return inside > 0.6 * (w.t1 - w.t0);
    })
    .map((w) => ({ text: w.text, start: Math.max(w.t0, from) - from, end: Math.min(w.t1, to) - from }));
}

function runLecture(words: AbsWord[], chunks: number, D = 5, opts: { preroll?: boolean; skip?: number[] } = {}) {
  let state: SttState = emptyState();
  const out: string[] = [];
  const utterances: string[] = [];
  for (let seq = 1; seq <= chunks; seq++) {
    const final = seq === chunks;
    const chunkStart = (seq - 1) * D;
    let r: { committed: PlacedWord[]; state: SttState };
    if (opts.skip?.includes(seq)) {
      r = skipChunk(state, seq);
    } else {
      const P = opts.preroll !== false && state.prevSeq === seq - 1 && seq > 1 ? PREROLL_SEC : 0;
      r = stitch(state, { seq, durationSec: D, prerollSec: P, final }, hear(words, chunkStart - P, chunkStart + D));
    }
    out.push(...r.committed.map((w) => w.text));
    const s = segment(r.state, r.committed, { currentSeq: seq, force: final, trailingPauseSec: 0, fallbackLanguage: "en" });
    state = s.state;
    utterances.push(...s.utterances.map((u) => u.text));
  }
  return { out, utterances };
}

const TEXT =
  "Today we discuss thermodynamics. The first law states that energy is conserved. " +
  "Entropy, however, always increases in an isolated system. Consider a heat engine operating between two reservoirs. " +
  "Its efficiency is bounded by the Carnot limit. Questions? Let us continue with examples from refrigeration cycles.";

Deno.test("every word is committed exactly once across chunk boundaries (with pre-roll)", () => {
  const words = lecture(TEXT);
  const chunks = Math.ceil(words[words.length - 1].t1 / 5);
  const { out } = runLecture(words, chunks);
  assertEquals(out, words.map((w) => w.text));
});

Deno.test("without pre-roll a boundary word may be lost, never duplicated", () => {
  const words = lecture(TEXT);
  const chunks = Math.ceil(words[words.length - 1].t1 / 5);
  const { out } = runLecture(words, chunks, 5, { preroll: false });
  assertEquals(new Set(out).size <= out.length, true);
  for (let i = 1; i < out.length; i++) assert(!(out[i] === out[i - 1] && out[i] !== "the"), `repeat at ${i}`);
});

Deno.test("words at every possible boundary offset are kept once", () => {
  for (let offset = 0; offset < 0.5; offset += 0.05) {
    const words = lecture(TEXT, 0.2 + offset);
    const chunks = Math.ceil(words[words.length - 1].t1 / 5);
    const { out } = runLecture(words, chunks);
    assertEquals(out, words.map((w) => w.text), `offset ${offset.toFixed(2)}`);
  }
});

Deno.test("utterances are whole sentences and reproduce the lecture text", () => {
  const words = lecture(TEXT);
  const chunks = Math.ceil(words[words.length - 1].t1 / 5);
  const { utterances } = runLecture(words, chunks);
  assertEquals(utterances.join(" "), TEXT);
  for (const u of utterances.slice(0, -1)) assert(/[.?!]$/.test(u) || u.split(" ").length > 20, u);
});

Deno.test("a skipped chunk commits the held words and breaks continuity", () => {
  const words = lecture(TEXT);
  const chunks = Math.ceil(words[words.length - 1].t1 / 5);
  const { out } = runLecture(words, chunks, 5, { skip: [3] });
  // Words only audible inside chunk 3 are gone; nothing is repeated.
  const keys = out.map((w, i) => `${w}#${out.slice(0, i).filter((x) => x === w).length}`);
  assertEquals(new Set(keys).size, out.length);
  assert(out.length < words.length);
});

Deno.test("held words stay within the guard window", () => {
  const words: Word[] = [
    { text: "alpha", start: 3.0, end: 3.4 },
    { text: "beta", start: 4.5, end: 4.9 },
  ];
  const r = stitch(emptyState(), { seq: 1, durationSec: 5, prerollSec: 0, final: false }, words);
  assertEquals(r.committed.map((w) => w.text), ["alpha"]);
  assertEquals(r.state.held.map((w) => w.text), ["beta"]);
  assert(r.state.prevCutSec <= 5 - GUARD_SEC);
});

Deno.test("an unfinished sentence is carried, then forced out after two chunks", () => {
  // Continuous speech with no punctuation: 0.4 s per word, no pauses, filling each 5 s chunk.
  const words = (seq: number, n: number): PlacedWord[] =>
    Array.from({ length: n }, (_, i) => ({ text: `w${seq}_${i}`, seq, start: i * 0.4, end: i * 0.4 + 0.35 }));
  const opts = (seq: number) => ({ currentSeq: seq, force: false, trailingPauseSec: 0, fallbackLanguage: null });
  let s = segment(emptyState(), words(1, 12), opts(1));
  assertEquals(s.utterances.length, 0);
  s = segment(s.state, words(2, 12), opts(2));
  assertEquals(s.utterances.length, 0);
  assertEquals(s.state.carry.length, 24);
  s = segment(s.state, words(3, 3), opts(3)); // carry started in chunk 1: too old now
  assertEquals(s.utterances.length, 1);
  assertEquals(s.utterances[0].words, 27);
  assertEquals(s.state.carry.length, 0);
});

Deno.test("a pause at the end of a chunk flushes the carry", () => {
  const words: PlacedWord[] = [{ text: "so", seq: 4, start: 0.2, end: 0.4 }, { text: "anyway", seq: 4, start: 0.5, end: 0.9 }];
  const s = segment(emptyState(), words, { currentSeq: 4, force: false, trailingPauseSec: 2.5, fallbackLanguage: "en" });
  assertEquals(s.utterances.map((u) => [u.text, u.language, u.startSeq, u.startMs, u.endMs]), [["so anyway", "en", 4, 200, 900]]);
});

Deno.test("utterance language is the majority word language, normalised", () => {
  const words: PlacedWord[] = [
    { text: "Goeie", seq: 1, start: 0, end: 0.3, language: "af" },
    { text: "môre", seq: 1, start: 0.4, end: 0.7, language: "af-ZA" },
    { text: "everyone.", seq: 1, start: 0.8, end: 1.2, language: "en" },
  ];
  const s = segment(emptyState(), words, { currentSeq: 1, force: false, trailingPauseSec: 0, fallbackLanguage: "en" });
  assertEquals(s.utterances[0].language, "af");
  assertEquals(s.utterances[0].text, "Goeie môre everyone.");
});

Deno.test("a sentence that waits too long is split at its last full stop, not mid-sentence", () => {
  // Continuous speech: each chunk's words run right up to the chunk's end.
  const mk = (seq: number, text: string, fromStart = false): PlacedWord[] => {
    const ws = text.split(" ");
    const t0 = (i: number) => (fromStart ? i * 0.4 : 5 - (ws.length - i) * 0.4);
    return ws.map((t, i) => ({ text: t, seq, start: t0(i), end: t0(i) + 0.35 }));
  };
  const opts = (seq: number) => ({ currentSeq: seq, force: false, trailingPauseSec: 0, fallbackLanguage: null });
  // Field case 2026-11-12: "For example, ... mechanical work. The efficiency ... of the heat" was
  // sent as one utterance and "is converted into work." as the next.
  let s = segment(emptyState(), mk(3, "For example, in a heat engine, chemical"), opts(3));
  s = segment(s.state, mk(4, "energy in the fuel becomes heat, and part of that heat becomes mechanical"), opts(4));
  s = segment(s.state, mk(5, "work. The efficiency of the engine tells us what fraction of the heat"), opts(5));
  assertEquals(s.utterances.map((u) => u.text), ["For example, in a heat engine, chemical energy in the fuel becomes heat, and part of that heat becomes mechanical work."]);
  s = segment(s.state, mk(6, "is converted into work. Please write", true), opts(6));
  assertEquals(s.utterances.map((u) => u.text), ["The efficiency of the engine tells us what fraction of the heat is converted into work."]);
  assertEquals(s.state.carry.map((w) => w.text), ["Please", "write"]);
});
