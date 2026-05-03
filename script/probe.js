#!/usr/bin/env bun
// scripts/probe.js
//
// Load the probe.node addon in the CURRENT runtime (Bun or Node) and
// attempt dlsym(RTLD_DEFAULT, ...) on each candidate mangled symbol.
//
// Usage:
//   bun  scripts/probe.js [additional-candidate-file]
//   node scripts/probe.js [additional-candidate-file]
//
// With no args, loads candidates from:
//   candidates/known.txt           (hand-picked baseline + guesses)
//   candidates/extracted.txt       (output of extract-jsc-symbols.sh)
//   candidates/generated.txt       (output of generate-candidates.sh)
//
// Emits a single JSON object on stdout.

'use strict';

const path = require('node:path');
const fs = require('node:fs');

const root = path.resolve(__dirname, '..');
const addonPath = path.join(root, 'script', 'probe_lazy.node');

if (!fs.existsSync(addonPath)) {
  console.error(`addon not built: ${addonPath}`);
  console.error(`run: make`);
  process.exit(2);
}

const probe = require(addonPath);

const runtime =
  typeof Bun !== 'undefined' ? `bun ${Bun.version}` :
  typeof Deno !== 'undefined' ? `deno ${Deno.version.deno}` :
  `node ${process.version}`;

// --- load candidates ---
const defaultFiles = [
  path.join(root, 'script', 'candidates', 'known.txt'),
  path.join(root, 'script', 'candidates', 'extracted.txt'),
  path.join(root, 'script', 'candidates', 'generated.txt'),
];
const extraFile = process.argv[2];
const files = extraFile
  ? [...defaultFiles, path.resolve(extraFile)]
  : defaultFiles;

const filesUsed = [];
const symbols = new Set();
for (const f of files) {
  if (!fs.existsSync(f)) continue;
  filesUsed.push(f);
  const lines = fs.readFileSync(f, 'utf8').split('\n');
  for (let line of lines) {
    line = line.trim();
    if (!line || line.startsWith('#')) continue;
    // Files from extract-jsc-symbols.sh have "<mangled>\t<demangled>";
    // take the first whitespace-separated token.
    const mangled = line.split(/\s+/)[0];
    if (mangled) symbols.add(mangled);
  }
}

if (symbols.size === 0) {
  console.error('no candidate symbols loaded');
  console.error('run: bash script/extract-jsc-symbols.sh > script/candidates/extracted.txt');
  console.error('(or) bash script/generate-candidates.sh > script/candidates/generated.txt');
  process.exit(2);
}

const symbolArray = Array.from(symbols);

// --- run probes ---
const env = probe.environmentInfo();
const results = probe.probeSymbols(symbolArray);

const entries = Object.entries(results);
const resolved = entries.filter(([, v]) => v.resolved);
const unresolved = entries.filter(([, v]) => !v.resolved);

// Classify the overall outcome for quick human scanning.
const baselineOk = Object.values(env.baseline_c_symbols || {}).every(Boolean);
let verdict;
if (!baselineOk) {
  verdict = 'probe_broken';
} else if (resolved.length === 0) {
  verdict = 'no_jsc_symbols_reachable';
} else if (resolved.some(([k]) => k.includes('JSC') || k.includes('Bun__'))) {
  verdict = 'jsc_or_bun_symbols_reachable';
} else {
  // Only baseline C symbols resolved.
  verdict = 'only_baseline_reachable';
}

const summary = {
  runtime,
  platform: env.platform,
  candidate_files: filesUsed,
  baseline_c_symbols: env.baseline_c_symbols,
  totals: {
    candidates: symbolArray.length,
    resolved: resolved.length,
    unresolved: unresolved.length,
  },
  verdict,
  resolved: resolved.map(([k, v]) => ({ symbol: k, address: v.address })),
  unresolved_sample: unresolved.slice(0, 20).map(([k]) => k),
};

console.log(JSON.stringify(summary, null, 2));

// Exit non-zero if nothing useful resolved; useful in CI matrix.
process.exit(verdict === 'jsc_or_bun_symbols_reachable' ? 0 : 1);
