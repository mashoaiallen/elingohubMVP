#!/usr/bin/env node
// Builds the LittleFS image from firmware/esp32_hub/data and flashes it to the Hub.
//
//   node tools/flash-web.mjs --port COM5          build + flash
//   node tools/flash-web.mjs --build-only         just build build/littlefs.bin
//
// Uses mklittlefs and esptool from the installed ESP32 Arduino core. Partition layout matches
// PartitionScheme=app5M_little24M_32MB (core file large_littlefs_32MB.csv): "spiffs" data
// partition at 0x910000, size 0x16E0000. The firmware mounts it with LittleFS.begin().
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { parseArgs, REPO_DIR } from "./lib.mjs";

const OFFSET = "0x910000";
const SIZE = "0x16E0000";
const args = parseArgs(process.argv.slice(2));

function arduinoTool(name, exe) {
  const base = process.env.ARDUINO15 ||
    (process.platform === "win32"
      ? path.join(process.env.LOCALAPPDATA || "", "Arduino15")
      : process.platform === "darwin"
        ? path.join(os.homedir(), "Library", "Arduino15")
        : path.join(os.homedir(), ".arduino15"));
  const dir = path.join(base, "packages", "esp32", "tools", name);
  if (!existsSync(dir)) throw new Error(`${name} not found under ${dir} (install the esp32 core)`);
  const versions = readdirSync(dir).sort().reverse();
  for (const v of versions) {
    const candidate = path.join(dir, v, process.platform === "win32" ? `${exe}.exe` : exe);
    if (existsSync(candidate)) return candidate;
  }
  throw new Error(`${exe} not found in ${dir}`);
}

const dataDir = path.join(REPO_DIR, "firmware", "esp32_hub", "data");
const outDir = path.join(REPO_DIR, "build");
mkdirSync(outDir, { recursive: true });
const image = path.join(outDir, "littlefs.bin");

execFileSync(arduinoTool("mklittlefs", "mklittlefs"), ["-c", dataDir, "-b", "4096", "-p", "256", "-s", SIZE, image], {
  stdio: "inherit",
});
console.log(`Built ${path.relative(REPO_DIR, image)} from ${path.relative(REPO_DIR, dataDir)}`);

if (args["build-only"]) process.exit(0);
if (typeof args.port !== "string") {
  console.error("Pass --port <COMx or /dev/ttyACM0> to flash, or --build-only.");
  process.exit(2);
}
execFileSync(
  arduinoTool("esptool_py", "esptool"),
  ["--chip", "esp32s3", "--port", args.port, "--baud", "921600", "write-flash", OFFSET, image],
  { stdio: "inherit" },
);
console.log("Web files flashed. Reset the Hub if it was running.");
