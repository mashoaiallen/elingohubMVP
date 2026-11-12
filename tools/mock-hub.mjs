#!/usr/bin/env node
// Local stand-in for the Hub's web server, for developing/testing the student page in a
// browser without hardware. Serves firmware/esp32_hub/data and the same /api/* endpoints,
// producing a synthetic 5 s tone segment per language that has listeners.
//
//   node tools/mock-hub.mjs [--port 8765] [--no-session]
import { createServer } from "node:http";
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { makeWav, parseArgs, REPO_DIR } from "./lib.mjs";

const args = parseArgs(process.argv.slice(2));
const port = Number(args.port || 8765);
const sessionActive = !args["no-session"];
const dataDir = path.join(REPO_DIR, "firmware", "esp32_hub", "data");
const languages = [
  { code: "en", name: "English", nativeName: "English", freq: 330 },
  { code: "af", name: "Afrikaans", nativeName: "Afrikaans", freq: 392 },
  { code: "st", name: "Sesotho (Southern Sotho)", nativeName: "Sesotho", freq: 440 },
  { code: "nso", name: "Sepedi (Northern Sotho)", nativeName: "Sepedi", freq: 494 },
  { code: "ss", name: "siSwati", nativeName: "siSwati", freq: 523 },
];
const listeners = new Map(); // id -> { lang, lastSeen }
const segments = new Map();  // lang -> [{ seq, durationMs, bytes }]
const wavs = new Map();
let seq = 0;

setInterval(() => {
  const now = Date.now();
  for (const [id, l] of listeners) if (now - l.lastSeen > 30000) listeners.delete(id);
  if (!sessionActive) return;
  seq++;
  const active = new Set([...listeners.values()].map((l) => l.lang));
  for (const lang of active) {
    const info = languages.find((l) => l.code === lang);
    const wav = makeWav(5000, info.freq, 3000);
    wavs.set(`${lang}:${seq}`, wav);
    const list = segments.get(lang) ?? [];
    list.push({ seq, durationMs: 5000, bytes: wav.length });
    while (list.length > 12) wavs.delete(`${lang}:${list.shift().seq}`);
    segments.set(lang, list);
  }
}, 5000);

const json = (res, status, body) => {
  res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  res.end(JSON.stringify(body));
};
const readBody = (req) => new Promise((resolve) => {
  let s = "";
  req.on("data", (c) => (s += c));
  req.on("end", () => { try { resolve(JSON.parse(s || "{}")); } catch { resolve(null); } });
});
const latest = (lang) => (segments.get(lang)?.at(-1)?.seq ?? -1);
const types = { ".html": "text/html; charset=utf-8", ".js": "application/javascript", ".css": "text/css" };

createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const p = url.pathname;
  if (p === "/api/status") {
    return json(res, 200, {
      hub: { name: "Mock Hub", firmware: "mock" },
      session: { active: sessionActive, translating: segments.size > 0, degraded: false, elapsedS: 0 },
      cloud: { online: true }, upstream: { connected: true },
      network: { clients: listeners.size, maxClients: 10 }, setupMode: false,
      languages: languages.map((l) => ({ code: l.code, name: l.name, nativeName: l.nativeName,
        listeners: [...listeners.values()].filter((x) => x.lang === l.code).length, latestSeq: latest(l.code) })),
    });
  }
  if (p === "/api/join" && req.method === "POST") {
    const b = await readBody(req);
    if (!b || !/^[A-Za-z0-9_-]{8,64}$/.test(b.listenerId ?? "")) return json(res, 400, { error: "invalid_listener" });
    if (!languages.some((l) => l.code === b.language)) return json(res, 400, { error: "unknown_language" });
    listeners.set(b.listenerId, { lang: b.language, lastSeen: Date.now() });
    return json(res, 200, { listenerId: b.listenerId, language: b.language, sessionActive, latestSeq: latest(b.language) });
  }
  if (p === "/api/heartbeat" && req.method === "POST") {
    const b = await readBody(req);
    const l = listeners.get(b?.listenerId);
    if (!l) return json(res, 404, { error: "unknown_listener", message: "Please join again." });
    l.lastSeen = Date.now();
    return json(res, 200, { ok: true, language: l.lang, sessionActive, latestSeq: latest(l.lang) });
  }
  if (p === "/api/leave" && req.method === "POST") {
    const b = await readBody(req);
    return json(res, 200, { ok: true, removed: listeners.delete(b?.listenerId) });
  }
  if (p === "/api/segments") {
    const lang = url.searchParams.get("lang");
    const after = Number(url.searchParams.get("after") ?? -1);
    return json(res, 200, { language: lang, sessionActive, latestSeq: latest(lang),
      segments: (segments.get(lang) ?? []).filter((s) => s.seq > after) });
  }
  if (p === "/api/audio") {
    const wav = wavs.get(`${url.searchParams.get("lang")}:${url.searchParams.get("seq")}`);
    if (!wav) return json(res, 404, { error: "segment_gone" });
    res.writeHead(200, { "Content-Type": "audio/wav", "Cache-Control": "no-store" });
    return res.end(Buffer.from(wav));
  }
  if (p.startsWith("/api/")) return json(res, 404, { error: "not_found" });
  const file = path.join(dataDir, p === "/" ? "index.html" : p.replace(/^\/+/, ""));
  if (file.startsWith(dataDir) && existsSync(file)) {
    res.writeHead(200, { "Content-Type": types[path.extname(file)] ?? "application/octet-stream" });
    return res.end(readFileSync(file));
  }
  res.writeHead(302, { Location: "/" });
  res.end();
}).listen(port, () => console.log(`Mock Hub on http://localhost:${port} (session ${sessionActive ? "active" : "inactive"})`));
