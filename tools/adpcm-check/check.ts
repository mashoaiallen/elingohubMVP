// Proves the Hub firmware's ADPCM encoder (C++) and the cloud's (TypeScript) produce identical
// bytes, so the cloud and the student page decode exactly what the Hub sends.
//   deno run -A tools/adpcm-check/check.ts <path-to-adpcm-encode-binary>
import { encodeAdpcmWav } from "../../supabase/functions/_shared/audio.ts";

const exe = Deno.args[0];
const dir = await Deno.makeTempDir();
let failures = 0;
const cases: [string, (i: number) => number, number][] = [
  ["speech-like 5 s chunk", (i) => 6000 * Math.sin(i / 14) * (0.5 + 0.5 * Math.sin(i / 3000)) + 2000 * Math.sin(i / 2.1), 80000],
  ["full-scale square", (i) => (i % 37 < 18 ? 32767 : -32768), 5000],
  ["silence", () => 0, 3000],
  ["tail chunk (partial block)", (i) => 3000 * Math.sin(i / 9), 2 * 1017 + 334],
  ["one sample", () => 1234, 1],
];
for (const [name, f, n] of cases) {
  const pcm = new Int16Array(n);
  for (let i = 0; i < n; i++) pcm[i] = Math.max(-32768, Math.min(32767, Math.round(f(i))));
  await Deno.writeFile(`${dir}/in.pcm`, new Uint8Array(pcm.buffer));
  const p = await new Deno.Command(exe, { args: [`${dir}/in.pcm`, `${dir}/out.wav`] }).output();
  const c = await Deno.readFile(`${dir}/out.wav`);
  const ts = encodeAdpcmWav({ sampleRate: 16000, samples: pcm });
  const same = p.code === 0 && c.length === ts.length && c.every((v, i) => v === ts[i]);
  if (!same) failures++;
  console.log(`${same ? "ok    " : "FAIL  "} ${name}: C++ ${c.length} bytes, TS ${ts.length} bytes`);
}
console.log(failures ? `${failures} FAILED` : "C++ and TypeScript encoders are byte-identical");
Deno.exit(failures ? 1 : 0);
