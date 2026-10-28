// Builds the npm packages from the release archives.
//
//   node scripts/npm-packages.mjs <version> <dist dir> <output dir>
//
// <dist dir> holds the archives the release workflow makes (culprit-<version>-linux-x64.tar.gz, ...).
// The output has one folder per package, ready for `npm publish`: a package per platform with the binary
// in it, and culprit-cli, which lists them all as optional dependencies so npm installs the right one.

import { execFileSync } from "node:child_process";
import { chmodSync, cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const [version, distArg, outArg] = process.argv.slice(2);
if (!version || !distArg || !outArg) {
  console.error("usage: node scripts/npm-packages.mjs <version> <dist dir> <output dir>");
  process.exit(2);
}
const dist = resolve(distArg);
const out = resolve(outArg);

const targets = [
  { release: "linux-x64", name: "culprit-cli-linux-x64", os: ["linux"], cpu: ["x64"] },
  { release: "linux-arm64", name: "culprit-cli-linux-arm64", os: ["linux"], cpu: ["arm64"] },
  { release: "macos-universal", name: "culprit-cli-darwin-universal", os: ["darwin"], cpu: ["x64", "arm64"] },
  { release: "windows-x64", name: "culprit-cli-win32-x64", os: ["win32"], cpu: ["x64", "arm64"], windows: true },
];

const main = JSON.parse(readFileSync(join(root, "packaging/npm/culprit-cli/package.json"), "utf8"));
const shared = {
  version,
  license: main.license,
  author: main.author,
  homepage: main.homepage,
  repository: main.repository,
};

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });

for (const t of targets) {
  const base = `culprit-${version}-${t.release}`;
  const archive = join(dist, base + (t.windows ? ".zip" : ".tar.gz"));
  if (!existsSync(archive)) throw new Error(`missing ${archive}`);

  const dir = join(out, t.name);
  const bin = join(dir, "bin");
  mkdirSync(bin, { recursive: true });
  if (t.windows) execFileSync("unzip", ["-q", "-o", archive, "-d", bin]);
  else execFileSync("tar", ["-xzf", archive, "-C", bin]);

  const exe = join(bin, t.windows ? "culprit.exe" : "culprit");
  if (!existsSync(exe)) throw new Error(`${archive} has no ${t.windows ? "culprit.exe" : "culprit"}`);
  if (!t.windows) chmodSync(exe, 0o755);

  writeFileSync(
    join(dir, "package.json"),
    JSON.stringify(
      {
        name: t.name,
        ...shared,
        description: `The culprit binary for ${t.os[0]} ${t.cpu.join("/")}. Install culprit-cli instead of this.`,
        os: t.os,
        cpu: t.cpu,
        files: ["bin"],
        preferUnplugged: true,
      },
      null,
      2,
    ) + "\n",
  );
  writeFileSync(
    join(dir, "README.md"),
    `# ${t.name}\n\nThe prebuilt [culprit](https://github.com/amirizalrahmat0799/culprit) binary for ` +
      `${t.os[0]} ${t.cpu.join("/")}.\n\nInstall [culprit-cli](https://www.npmjs.com/package/culprit-cli) ` +
      `instead; it picks the right binary for your machine.\n`,
  );
  cpSync(join(root, "LICENSE"), join(dir, "LICENSE"));
  console.log(`${t.name}@${version}`);
}

const mainDir = join(out, "culprit-cli");
cpSync(join(root, "packaging/npm/culprit-cli"), mainDir, { recursive: true });
cpSync(join(root, "LICENSE"), join(mainDir, "LICENSE"));
chmodSync(join(mainDir, "bin/culprit.js"), 0o755);
main.version = version;
main.optionalDependencies = Object.fromEntries(targets.map((t) => [t.name, version]));
writeFileSync(join(mainDir, "package.json"), JSON.stringify(main, null, 2) + "\n");
console.log(`culprit-cli@${version}`);
