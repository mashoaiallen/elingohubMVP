#!/usr/bin/env node
// Runs the database tests against the linked Supabase project inside ONE transaction that is
// always rolled back: nothing persists and no Edge Function is invoked (pg_net requests queued
// inside the transaction are discarded with it). Pending migrations can be included, so tests
// can run before a migration is applied.
//
//   node tools/run-sql-tests.mjs [--with-migration supabase/migrations/<file>.sql] <test.sql>...
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { parseArgs } from "./lib.mjs";

const args = parseArgs(process.argv.slice(2));
const parts = ["begin;"];
if (typeof args["with-migration"] === "string") parts.push(readFileSync(args["with-migration"], "utf8"));
let failures = 0;
for (const test of args._) {
  const sql = [...parts, readFileSync(test, "utf8"), "rollback;"].join("\n");
  const file = path.join(mkdtempSync(path.join(tmpdir(), "elingo-sql-")), "run.sql");
  writeFileSync(file, sql);
  let out;
  try {
    out = execFileSync("supabase", ["db", "query", "--remote", "--yes", "-f", file], { encoding: "utf8", maxBuffer: 1 << 24, stdio: ["ignore", "pipe", "ignore"] });
  } catch (e) {
    const msg = String(e.stdout ?? e.message);
    console.log(`not ok - ${test} aborted: ${msg.match(/ERROR:.*?(?=\\n|"})/)?.[0] ?? msg.slice(0, 600)}`);
    failures++;
    continue;
  }
  const json = JSON.parse(out.slice(out.indexOf("{")));
  const lines = json.rows.map((r) => Object.values(r)[0]);
  for (const l of lines) console.log(l);
  failures += lines.filter((l) => String(l).startsWith("not ok")).length;
}
console.log(failures ? `\n${failures} FAILED` : "\nall passed");
process.exit(failures ? 1 : 0);
