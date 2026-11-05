// Builds out/translate/sumatrapdf-translate.exe, the offline translation helper
// (src/tools/translate/translate.cpp). It links the official CTranslate2 DLL
// from the PyPI wheel (the same one the install command downloads) and a
// SentencePiece built here. Everything it needs is fetched into .work/translate.
//
//   bun cmd/build-translate.ts
//
// Upload the resulting exe to a release and put its SHA-256 (printed at the end)
// in the install constants of src/shared/Translate_win.cpp.
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { findToolchain } from "./helper/ng-toolchain";

const root = join(import.meta.dir, "..");
const work = join(root, ".work", "translate");
const outDir = join(root, "out", "translate");

// pinned inputs
const ct2Tag = "v4.8.5";
const spmTag = "v0.2.0";
const wheelUrl =
  "https://files.pythonhosted.org/packages/20/b9/3ae145ed28a3d11ddfe5e3e98c8cd56ffef3c914e61b65790364e9e602c0/ctranslate2-4.8.5-cp313-cp313-win_amd64.whl";
const wheelSha256 = "9667fa5589dec8b57eaa31d4602935259092e4185e016afb0c7d4a5efaaaedfb";

function fail(msg: string): never {
  console.error(msg);
  process.exit(1);
}

function sha256(path: string): string {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

function run(cmd: string[], cwd: string, env: Record<string, string>) {
  const r = Bun.spawnSync(cmd, { cwd, env: { ...process.env, ...env }, stdout: "inherit", stderr: "inherit" });
  if (r.exitCode !== 0) {
    fail(`failed: ${cmd.join(" ")}`);
  }
}

function clone(url: string, tag: string, dir: string) {
  if (existsSync(dir)) {
    return;
  }
  run(["git", "clone", "--quiet", "--depth", "1", "--branch", tag, url, dir], root, {});
}

async function fetchWheel(dest: string) {
  if (existsSync(dest) && sha256(dest) === wheelSha256) {
    return;
  }
  const rsp = await fetch(wheelUrl);
  if (!rsp.ok) {
    fail(`download failed: ${wheelUrl}`);
  }
  writeFileSync(dest, Buffer.from(await rsp.arrayBuffer()));
  if (sha256(dest) !== wheelSha256) {
    fail("the wheel does not match its pinned SHA-256");
  }
}

// ctranslate2.dll ships without an import library
function importLib(env: Record<string, string>, dll: string, outLib: string) {
  const dump = Bun.spawnSync(["dumpbin", "/exports", dll], { env: { ...process.env, ...env } });
  const names: string[] = [];
  for (const line of dump.stdout.toString().split(/\r?\n/)) {
    const m = line.match(/^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)/);
    if (m) {
      names.push(m[1]);
    }
  }
  if (names.length === 0) {
    fail("no exports found in ctranslate2.dll");
  }
  const def = join(work, "ctranslate2.def");
  writeFileSync(def, ["LIBRARY ctranslate2", "EXPORTS", ...names.map((n) => `  ${n}`)].join("\n"));
  run(["lib", "/nologo", `/def:${def}`, "/machine:x64", `/out:${outLib}`], work, env);
}

async function main() {
  const tc = findToolchain(root, "win", false, fail);
  const env = tc.env;
  mkdirSync(work, { recursive: true });
  mkdirSync(outDir, { recursive: true });

  const ct2 = join(work, "ct2");
  const spm = join(work, "spm");
  clone("https://github.com/OpenNMT/CTranslate2.git", ct2Tag, ct2);
  clone("https://github.com/google/sentencepiece.git", spmTag, spm);

  const spmLib = join(spm, "build", "src", "sentencepiece.lib");
  if (!existsSync(spmLib)) {
    run(
      [
        "cmake",
        "-S",
        spm,
        "-B",
        join(spm, "build"),
        "-G",
        "NMake Makefiles",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DSPM_ENABLE_SHARED=OFF",
        "-DSPM_BUILD_TEST=OFF",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
      ],
      work,
      env,
    );
    run(["cmake", "--build", join(spm, "build"), "--target", "sentencepiece-static", "-j", "8"], work, env);
  }

  const wheel = join(work, "ctranslate2.whl");
  await fetchWheel(wheel);
  const dllDir = join(work, "wheel");
  mkdirSync(dllDir, { recursive: true });
  // Windows' own bsdtar: Git's GNU tar reads "S:" as a remote host
  const tar = join(process.env.SystemRoot ?? "C:\\Windows", "System32", "tar.exe");
  run([tar, "-xf", wheel, "-C", dllDir, "ctranslate2/ctranslate2.dll", "ctranslate2/libiomp5md.dll"], work, env);
  const dll = join(dllDir, "ctranslate2", "ctranslate2.dll");
  const lib = join(work, "ctranslate2.lib");
  if (!existsSync(lib)) {
    importLib(env, dll, lib);
  }

  const exe = join(outDir, "sumatrapdf-translate.exe");
  run(
    [
      "cl",
      "/nologo",
      "/utf-8",
      "/std:c++17",
      "/O2",
      "/EHsc",
      "/MD",
      "/DNOMINMAX",
      join(root, "src", "tools", "translate", "translate.cpp"),
      `/I${join(ct2, "include")}`,
      `/I${join(spm, "src")}`,
      `/Fo${work}\\`,
      `/Fe:${exe}`,
      "/link",
      `/LIBPATH:${work}`,
      `/LIBPATH:${join(spm, "build", "src")}`,
      "ctranslate2.lib",
      "sentencepiece.lib",
    ],
    work,
    env,
  );
  console.log(`built ${exe}`);
  console.log(`sha256 ${sha256(exe)}`);
  // run it from a folder that has the DLLs, to test it
  copyFileSync(dll, join(outDir, "ctranslate2.dll"));
  copyFileSync(join(dllDir, "ctranslate2", "libiomp5md.dll"), join(outDir, "libiomp5md.dll"));
}

await main();
