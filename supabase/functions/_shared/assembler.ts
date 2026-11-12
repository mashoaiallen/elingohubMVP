// Transcript assembly: turns per-chunk Deepgram results into whole sentences ("utterances")
// without repeating or losing words at chunk boundaries. Pure functions; the session state is
// persisted in sessions.stt_state between chunks.
//
// Boundary handling
//   Chunk N is transcribed together with the last PREROLL seconds of chunk N-1 (when the two are
//   contiguous recordings), so a word cut in half by the boundary is heard whole in chunk N.
//   Chunk N-1 does not commit words that end within GUARD seconds of its end ("held" words);
//   chunk N then takes over from exactly where N-1 stopped committing (the "cut"), so every
//   word is committed exactly once. If N-1 and N are not contiguous (gap, reboot, skipped chunk),
//   the held words are committed as recognised in N-1.
//
// Sentence handling
//   Committed words collect in a carry until a sentence ends (. ? ! ...), the lecturer pauses,
//   or the carry gets too old/long. Each flush becomes one utterance, which is translated and
//   spoken once per active language. Unfinished sentences are never sent to translation.

export const PREROLL_SEC = 1.0;
export const GUARD_SEC = 0.35;
const EPS = 0.05;
export const PAUSE_FLUSH_SEC = 1.0; // trailing silence in a chunk that ends a thought
export const GAP_SPLIT_SEC = 1.2; // pause between two words that ends a thought
export const MAX_CARRY_CHUNKS = 2; // a sentence still unfinished after ~10 s is sent anyway
export const MAX_CARRY_WORDS = 45;

export interface Word {
  text: string; // punctuated form
  start: number; // seconds
  end: number;
  confidence?: number;
  language?: string;
}

/** A word placed in a chunk: times are relative to the start of chunk `seq`. */
export interface PlacedWord extends Word {
  seq: number;
}

export interface SttState {
  prevSeq: number | null;
  prevDurationSec: number;
  prevCutSec: number; // in the previous chunk: words ending up to here were committed
  held: Word[]; // previous chunk's uncommitted tail (times relative to that chunk)
  carry: PlacedWord[]; // committed words waiting for the end of their sentence
}

export interface Utterance {
  text: string;
  language: string | null;
  startSeq: number;
  startMs: number;
  endSeq: number;
  endMs: number;
  words: number;
}

export function emptyState(): SttState {
  return { prevSeq: null, prevDurationSec: 0, prevCutSec: 0, held: [], carry: [] };
}

export function normalizeState(raw: unknown): SttState {
  const s = (raw ?? {}) as Partial<SttState>;
  return {
    prevSeq: typeof s.prevSeq === "number" ? s.prevSeq : null,
    prevDurationSec: typeof s.prevDurationSec === "number" ? s.prevDurationSec : 0,
    prevCutSec: typeof s.prevCutSec === "number" ? s.prevCutSec : 0,
    held: Array.isArray(s.held) ? s.held : [],
    carry: Array.isArray(s.carry) ? s.carry : [],
  };
}

export interface ChunkInput {
  seq: number;
  durationSec: number;
  prerollSec: number; // 0 when no pre-roll audio was prepended
  final: boolean; // last chunk of the session: commit everything
}

/**
 * Commits this chunk's words. `words` are timed relative to the audio that was transcribed
 * (pre-roll included). Returns the committed words in order and the state for the next chunk
 * (carry is untouched here; see segment()).
 */
export function stitch(state: SttState, chunk: ChunkInput, words: Word[]): { committed: PlacedWord[]; state: SttState } {
  const D = chunk.durationSec;
  const P = chunk.prerollSec;
  const committed: PlacedWord[] = [];
  const prevSeq = state.prevSeq;
  const contiguous = P > 0 && prevSeq === chunk.seq - 1;
  let acceptFrom = -Infinity;

  if (contiguous) {
    const prevD = state.prevDurationSec;
    let lastEnd = state.prevCutSec - prevD; // relative to this chunk, <= 0
    for (const h of state.held) {
      // Held words that start before the pre-roll window were not heard again: keep them.
      if (h.start - prevD < -P + EPS) {
        committed.push({ ...h, seq: prevSeq! });
        lastEnd = Math.max(lastEnd, h.end - prevD);
      }
    }
    acceptFrom = Math.max(lastEnd, -P) - EPS;
  } else {
    if (prevSeq !== null) for (const h of state.held) committed.push({ ...h, seq: prevSeq });
    acceptFrom = -EPS; // any pre-roll words would repeat the held ones
  }

  const own = words
    .map((w) => ({ ...w, start: w.start - P, end: w.end - P }))
    .filter((w) => w.start >= acceptFrom)
    .sort((a, b) => a.start - b.start);

  const held: Word[] = [];
  let cut = 0;
  for (const w of own) {
    if (!chunk.final && w.end > D - GUARD_SEC) {
      held.push(w);
      continue;
    }
    if (held.length > 0) {
      held.push(w); // keep order: once holding starts, everything after is held too
      continue;
    }
    if (w.start < 0 && prevSeq !== null && contiguous) {
      committed.push({ ...w, seq: prevSeq, start: w.start + state.prevDurationSec, end: w.end + state.prevDurationSec });
    } else {
      committed.push({ ...w, seq: chunk.seq, start: Math.max(0, w.start), end: Math.max(0, w.end) });
    }
    cut = Math.max(cut, w.end);
  }

  return {
    committed,
    state: { prevSeq: chunk.seq, prevDurationSec: D, prevCutSec: Math.max(0, cut), held, carry: state.carry },
  };
}

/** A chunk that was not transcribed (silent, skipped, failed): what was pending is committed. */
export function skipChunk(state: SttState, seq: number): { committed: PlacedWord[]; state: SttState } {
  const committed = state.prevSeq === null ? [] : state.held.map((h) => ({ ...h, seq: state.prevSeq! }));
  return { committed, state: { prevSeq: seq, prevDurationSec: 0, prevCutSec: 0, held: [], carry: state.carry } };
}

const SENTENCE_END = /[.?!…。？！]["'”’)\]]*$/;

function joinWords(words: PlacedWord[]): string {
  return words.map((w) => w.text.trim()).filter(Boolean).join(" ").replace(/\s+([,.;:?!])/g, "$1");
}

function majorityLanguage(words: PlacedWord[]): string | null {
  const counts = new Map<string, number>();
  for (const w of words) if (w.language) counts.set(baseLanguage(w.language), (counts.get(baseLanguage(w.language)) ?? 0) + 1);
  let best: string | null = null;
  let n = 0;
  for (const [k, v] of counts) if (v > n) [best, n] = [k, v];
  return best;
}

export function baseLanguage(code: string): string {
  return code.toLowerCase().split(/[-_]/)[0];
}

function toUtterance(words: PlacedWord[], fallbackLanguage: string | null): Utterance {
  const first = words[0];
  const last = words[words.length - 1];
  return {
    text: joinWords(words),
    language: majorityLanguage(words) ?? fallbackLanguage,
    startSeq: first.seq,
    startMs: Math.round(first.start * 1000),
    endSeq: last.seq,
    endMs: Math.round(last.end * 1000),
    words: words.length,
  };
}

export interface SegmentOptions {
  currentSeq: number;
  force: boolean; // flush everything (session end, skipped chunk)
  trailingPauseSec: number; // silence after the last word of this chunk
  fallbackLanguage: string | null;
}

/** Moves committed words into the carry and returns the utterances that are complete. */
export function segment(state: SttState, committed: PlacedWord[], opts: SegmentOptions): { utterances: Utterance[]; state: SttState } {
  const all = [...state.carry, ...committed].filter((w) => w.text.trim().length > 0);
  const utterances: Utterance[] = [];
  let startIdx = 0;
  let lastEnd = -1;

  // Split at long pauses between words, and remember the last sentence end.
  for (let i = 0; i < all.length; i++) {
    if (i > startIdx) {
      const gap = absTime(all[i]) - absTime(all[i - 1], true);
      if (gap >= GAP_SPLIT_SEC) {
        utterances.push(toUtterance(all.slice(startIdx, i), opts.fallbackLanguage));
        startIdx = i;
        lastEnd = -1;
      }
    }
    if (SENTENCE_END.test(all[i].text.trim())) lastEnd = i;
  }

  let rest = all.slice(startIdx);
  const endInRest = lastEnd >= startIdx ? lastEnd - startIdx : -1;
  const tooOld = (w: PlacedWord[]) => w.length > 0 && w[0].seq <= opts.currentSeq - MAX_CARRY_CHUNKS;

  if (opts.force || opts.trailingPauseSec >= PAUSE_FLUSH_SEC) {
    // The lecturer stopped (pause, skipped chunk, session end): everything pending goes out.
    if (rest.length) utterances.push(toUtterance(rest, opts.fallbackLanguage));
    rest = [];
  } else {
    // Complete sentences go out; an unfinished one waits, unless it has waited too long or grown
    // too long, in which case it is sent without its end (better than holding speech back).
    if (endInRest >= 0) {
      utterances.push(toUtterance(rest.slice(0, endInRest + 1), opts.fallbackLanguage));
      rest = rest.slice(endInRest + 1);
    }
    if (tooOld(rest) || rest.length > MAX_CARRY_WORDS) {
      utterances.push(toUtterance(rest, opts.fallbackLanguage));
      rest = [];
    }
  }
  return { utterances, state: { ...state, carry: rest } };
}

// Approximate absolute time for pause detection (chunks are ~5 s; exact only within a run).
function absTime(w: PlacedWord, end = false): number {
  return w.seq * 5 + (end ? w.end : w.start);
}
