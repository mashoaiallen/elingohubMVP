// Shared helpers for the eLingo Hub dev tools (Node >= 20, no dependencies).
import { readFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const TOOLS_DIR = path.dirname(fileURLToPath(import.meta.url));
export const REPO_DIR = path.resolve(TOOLS_DIR, "..");

/** Reads tools/.env (gitignored): SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY, SUPABASE_SECRET_KEY. */
export function loadEnv() {
  const file = path.join(TOOLS_DIR, ".env");
  const env = { ...process.env };
  if (existsSync(file)) {
    for (const line of readFileSync(file, "utf8").split(/\r?\n/)) {
      const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)\s*$/);
      if (m && !(m[1] in process.env)) env[m[1]] = m[2];
    }
  }
  for (const k of ["SUPABASE_URL", "SUPABASE_PUBLISHABLE_KEY"]) {
    if (!env[k]) throw new Error(`${k} missing (tools/.env)`);
  }
  return env;
}

export function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("--")) {
      const key = a.slice(2);
      const next = argv[i + 1];
      if (next === undefined || next.startsWith("--")) out[key] = true;
      else { out[key] = next; i++; }
    } else out._.push(a);
  }
  return out;
}

export async function http(method, url, { headers = {}, body, raw = false } = {}) {
  const isBytes = body instanceof Uint8Array;
  const res = await fetch(url, {
    method,
    headers: { ...(body !== undefined && !isBytes ? { "Content-Type": "application/json" } : {}), ...headers },
    body: body === undefined ? undefined : isBytes ? body : JSON.stringify(body),
  });
  const buf = new Uint8Array(await res.arrayBuffer());
  let data = buf;
  if (!raw) {
    const text = new TextDecoder().decode(buf);
    try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  }
  return { status: res.status, ok: res.ok, data };
}

/** Minimal client that speaks exactly the protocol the ESP32 firmware uses. */
export class HubClient {
  constructor(url, publishableKey, email, password) {
    this.url = url.replace(/\/+$/, "");
    this.key = publishableKey;
    this.email = email;
    this.password = password;
    this.token = null;
  }
  async signIn() {
    const r = await http("POST", `${this.url}/auth/v1/token?grant_type=password`, {
      headers: { apikey: this.key },
      body: { email: this.email, password: this.password },
    });
    if (!r.ok) throw new Error(`sign-in failed: ${r.status} ${JSON.stringify(r.data)}`);
    this.token = r.data.access_token;
    return r.data;
  }
  headers() {
    return { apikey: this.key, Authorization: `Bearer ${this.token}` };
  }
  async rpc(fn, args = {}) {
    const r = await http("POST", `${this.url}/rest/v1/rpc/${fn}`, { headers: this.headers(), body: args });
    if (!r.ok) {
      const e = new Error(`${fn}: ${r.status} ${r.data?.message ?? JSON.stringify(r.data)}`);
      e.status = r.status;
      e.code = r.data?.code;
      throw e;
    }
    return r.data;
  }
  async upload(bucket, objectPath, bytes, contentType = "audio/wav") {
    const r = await http("POST", `${this.url}/storage/v1/object/${bucket}/${objectPath}`, {
      headers: { ...this.headers(), "Content-Type": contentType, "x-upsert": "true" },
      body: bytes,
    });
    if (!r.ok) throw new Error(`upload ${objectPath}: ${r.status} ${JSON.stringify(r.data)}`);
    return r.data;
  }
  async download(bucket, objectPath) {
    const r = await http("GET", `${this.url}/storage/v1/object/authenticated/${bucket}/${objectPath}`, {
      headers: this.headers(),
      raw: true,
    });
    if (!r.ok) throw new Error(`download ${objectPath}: ${r.status}`);
    return r.data;
  }
}

/** 16 kHz / 16-bit / mono WAV, same layout as the firmware's chunks. */
export function makeWav(durationMs, freqHz = 440, amplitude = 6000) {
  const sampleRate = 16000;
  const samples = Math.round((sampleRate * durationMs) / 1000);
  const buf = Buffer.alloc(44 + samples * 2);
  buf.write("RIFF", 0);
  buf.writeUInt32LE(36 + samples * 2, 4);
  buf.write("WAVE", 8);
  buf.write("fmt ", 12);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(sampleRate, 24);
  buf.writeUInt32LE(sampleRate * 2, 28);
  buf.writeUInt16LE(2, 32);
  buf.writeUInt16LE(16, 34);
  buf.write("data", 36);
  buf.writeUInt32LE(samples * 2, 40);
  for (let i = 0; i < samples; i++) {
    buf.writeInt16LE(Math.round(amplitude * Math.sin((2 * Math.PI * freqHz * i) / sampleRate)), 44 + i * 2);
  }
  return new Uint8Array(buf);
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
