#!/usr/bin/env node
"use strict";
// Starts the culprit binary from the platform package npm installed alongside this one
// (culprit-cli-linux-x64, culprit-cli-darwin-universal, ...). Only the package for this machine is installed.

const { spawn } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const PACKAGES = {
  "linux-x64": "culprit-cli-linux-x64",
  "linux-arm64": "culprit-cli-linux-arm64",
  "darwin-x64": "culprit-cli-darwin-universal",
  "darwin-arm64": "culprit-cli-darwin-universal",
  "win32-x64": "culprit-cli-windows-x64",
  "win32-arm64": "culprit-cli-windows-x64", // runs under Windows' x64 emulation
};

const SOURCE = "https://github.com/amirizalrahmat0799/culprit#install";

function fail(message) {
  process.stderr.write(`culprit: ${message}\n`);
  process.exit(1);
}

function binaryPath() {
  if (process.env.CULPRIT_BINARY) return process.env.CULPRIT_BINARY;

  const platform = `${process.platform}-${process.arch}`;
  const pkg = PACKAGES[platform];
  if (!pkg) fail(`there's no prebuilt binary for ${platform}. You can build culprit from source: ${SOURCE}`);

  let dir;
  try {
    dir = path.dirname(require.resolve(`${pkg}/package.json`));
  } catch {
    fail(
      `the package ${pkg} wasn't installed. npm skips it when optional dependencies are turned off ` +
        `(--omit=optional, --no-optional); install culprit-cli again without that option.`,
    );
  }
  const exe = path.join(dir, "bin", process.platform === "win32" ? "culprit.exe" : "culprit");

  // Some package managers drop the executable bit when they unpack.
  if (process.platform !== "win32") {
    try {
      fs.accessSync(exe, fs.constants.X_OK);
    } catch {
      try {
        fs.chmodSync(exe, 0o755);
      } catch {
        // spawn reports the problem below
      }
    }
  }
  return exe;
}

const exe = binaryPath();
// stdin, stdout and stderr are handed over as they are: culprit reads piped logs from stdin itself
// and opens the terminal for the keyboard.
const child = spawn(exe, process.argv.slice(2), { stdio: "inherit" });

child.on("error", (err) => fail(`couldn't start ${exe}: ${err.message}`));

// Ctrl+C reaches culprit from the terminal directly; other signals sent to this process are passed on.
process.on("SIGINT", () => {});
for (const signal of ["SIGTERM", "SIGHUP"]) {
  process.on(signal, () => child.kill(signal));
}

child.on("exit", (code, signal) => {
  if (signal) {
    process.removeAllListeners(signal);
    process.kill(process.pid, signal);
    return;
  }
  process.exit(code ?? 1);
});
