#!/usr/bin/env node
// Creates a Hub identity in Supabase: an Auth user (the Hub signs in as it) plus a public.hubs
// row. Run once per physical Hub from a laptop (a development tool, not needed to operate).
//
//   node tools/provision-hub.mjs --name "Lecture Hall 1" [--write-secrets]
//
// Needs tools/.env with SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY and SUPABASE_SECRET_KEY.
// Writes the credentials to tools/.hub-credentials-<slug>.json (gitignored). With
// --write-secrets it also fills the cloud fields of firmware/esp32_hub/secrets.h (gitignored).
import { randomBytes } from "node:crypto";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { http, loadEnv, parseArgs, REPO_DIR, TOOLS_DIR } from "./lib.mjs";

const args = parseArgs(process.argv.slice(2));
const name = typeof args.name === "string" ? args.name.trim() : "";
if (!name || name.length > 64) {
  console.error('Usage: node tools/provision-hub.mjs --name "Lecture Hall 1" [--write-secrets]');
  process.exit(2);
}
const env = loadEnv();
if (!env.SUPABASE_SECRET_KEY) throw new Error("SUPABASE_SECRET_KEY missing (tools/.env)");

const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 32) || "hub";
const suffix = randomBytes(4).toString("hex");
const email = `hub-${slug}-${suffix}@hubs.elingo-hub.app`;
const password = randomBytes(24).toString("base64url");
const admin = { apikey: env.SUPABASE_SECRET_KEY };

const user = await http("POST", `${env.SUPABASE_URL}/auth/v1/admin/users`, {
  headers: admin,
  body: { email, password, email_confirm: true, app_metadata: { elingo_role: "hub" }, user_metadata: { hub_name: name } },
});
if (!user.ok) {
  console.error("Creating the auth user failed:", user.status, user.data);
  process.exit(1);
}

const hub = await http("POST", `${env.SUPABASE_URL}/rest/v1/hubs`, {
  headers: { ...admin, Prefer: "return=representation" },
  body: { auth_user_id: user.data.id, name },
});
if (!hub.ok) {
  console.error("Creating the hubs row failed:", hub.status, hub.data);
  await http("DELETE", `${env.SUPABASE_URL}/auth/v1/admin/users/${user.data.id}`, { headers: admin });
  process.exit(1);
}

const creds = {
  hubId: hub.data[0].id,
  name,
  supabaseUrl: env.SUPABASE_URL,
  publishableKey: env.SUPABASE_PUBLISHABLE_KEY,
  hubEmail: email,
  hubPassword: password,
  createdAt: new Date().toISOString(),
};
const credFile = path.join(TOOLS_DIR, `.hub-credentials-${slug}.json`);
writeFileSync(credFile, JSON.stringify(creds, null, 2));
console.log(`Hub "${name}" provisioned (hub id ${creds.hubId}).`);
console.log(`Credentials saved to ${path.relative(REPO_DIR, credFile)} (gitignored).`);

if (args["write-secrets"]) {
  const secretsPath = path.join(REPO_DIR, "firmware", "esp32_hub", "secrets.h");
  const examplePath = path.join(REPO_DIR, "firmware", "esp32_hub", "secrets.example.h");
  let text = readFileSync(existsSync(secretsPath) ? secretsPath : examplePath, "utf8");
  const esc = (s) => s.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
  const set = (macro, value) => {
    const re = new RegExp(`(#define ${macro} )".*"`);
    text = re.test(text) ? text.replace(re, `$1"${esc(value)}"`) : `${text}\n#define ${macro} "${esc(value)}"\n`;
  };
  set("ELINGO_SUPABASE_URL", creds.supabaseUrl);
  set("ELINGO_SUPABASE_PUBLISHABLE_KEY", creds.publishableKey);
  set("ELINGO_HUB_EMAIL", creds.hubEmail);
  set("ELINGO_HUB_PASSWORD", creds.hubPassword);
  writeFileSync(secretsPath, text);
  console.log("Cloud credentials written to firmware/esp32_hub/secrets.h (gitignored).");
  console.log("A Hub that already booted keeps its stored values: run 'factory-reset' on its serial console,");
  console.log("or enter them with 'set supabase_url ...' etc.");
}
