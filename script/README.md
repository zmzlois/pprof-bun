# pprof-bun-probe

A scaffold for answering one question before committing to a JSC-native profiler
backend in `pprof-nodejs`:

> **Can an N-API addon loaded inside Bun resolve `JSC::*` symbols via `dlsym`?**

Everything here is a probe — no production code. Three layers, run bottom-up:

| Layer | Script | What it answers |
|---|---|---|
| Static inspection | `scripts/inspect-bun.sh` | Is the binary stripped? Are there JSC text references? What's linked? |
| Symbol extraction | `scripts/extract-jsc-symbols.sh` | If not stripped, what mangled JSC names are actually in the binary? |
| Candidate generation | `scripts/generate-candidates.sh` | If stripped, what mangled names *would* we want to look for? |
| Functional probe | `scripts/probe.js` (via `build/Release/probe.node`) | Do any of those names resolve via `dlsym(RTLD_DEFAULT, ...)` from inside Bun? |
| Matrix | `scripts/run-matrix.sh` | Same probes across a set of Bun versions |

## Prerequisites

- Node.js 18+ and npm (for `node-gyp`; the built addon loads in Bun regardless).
- `bun` on `$PATH` (or set `$BUN_BIN`).
- `clang++`, `nm`, `c++filt`, `strings` (stock on macOS/Linux).
- `jq` (optional, used by `run-matrix.sh` summary).

## Quickstart

```bash
npm install
npm run build           # compiles src/probe.cc -> build/Release/probe.node
npm run probe:all       # runs the full pipeline against `which bun`
```

Output lands in `results/latest-static.json` and `results/latest-functional.json`.

## Interpreting the verdict

`scripts/probe.js` prints a top-level `verdict` field. The possible values:

- **`probe_broken`** — a baseline libc symbol (`malloc`, `dlsym`, ...) failed to
  resolve. Something is wrong with the addon build itself. Investigate before
  trusting anything else.
- **`no_jsc_symbols_reachable`** — the addon loads, baseline resolves, but zero
  JSC/Bun candidates resolved. Bun's release binary doesn't expose what we
  need from its dynamic symbol table. Next step: small upstream PR to Bun
  adding `-Wl,-export_dynamic` (or selective `visibility("default")`) for the
  JSC entry points we care about.
- **`only_baseline_reachable`** — same as above in practice; the extra
  non-JSC candidates resolved but nothing load-bearing. Same next step.
- **`jsc_or_bun_symbols_reachable`** — at least one JSC or `Bun__*` symbol
  resolved. Proceed to VM acquisition testing (Layer 4 of the research plan).

The `classification` field in `results/latest-static.json` gives an independent
structural read:

- `red` — no `JSC::` strings in the binary at all. Something is very wrong.
- `dark` — JSC code present but symbol table stripped down to nothing.
  `extract-jsc-symbols.sh` returns empty; only the generated candidates are
  usable.
- `yellow` — symbols exist locally but aren't exported. `dlsym` may or may
  not reach them depending on link flags.
- `green` — meaningful export surface, high chance that raw dlsym works.

## Files

```
pprof-bun-probe/
├── binding.gyp                       build config for probe.node
├── package.json                      scripts + deps (node-addon-api, node-gyp)
├── src/
│   ├── probe.cc                      the N-API addon (dlsym probe)
│   └── candidates/
│       └── jsc_stub.cc               forward-decls -> undefined refs -> mangled names
├── candidates/
│   ├── known.txt                     baseline + speculative candidates (tracked)
│   ├── extracted.txt                 from real Bun binary (gitignored, regenerated)
│   └── generated.txt                 from jsc_stub.cc (gitignored, regenerated)
├── scripts/
│   ├── inspect-bun.sh                Probe A+B: static binary inspection
│   ├── extract-jsc-symbols.sh        pull JSC mangled names out of Bun binary
│   ├── generate-candidates.sh        compile stub, extract mangled names
│   ├── probe.js                      Probe C: runtime dlsym check
│   ├── probe-all.sh                  run everything against one Bun
│   └── run-matrix.sh                 run everything across many Bun versions
└── results/                          all probe output, gitignored
```

## Caveats

- The forward-declarations in `src/candidates/jsc_stub.cc` are educated
  guesses at JSC signatures. Mangled names they produce match real JSC
  only if signatures match. `extract-jsc-symbols.sh` against an unstripped
  Bun is the authoritative source; the stub is a fallback.
- `dlsym` resolving a symbol only proves the symbol's address is reachable.
  The next distinct question — whether we can construct a `JSC::VM*` to
  pass to that symbol — is Layer 4 and not covered here.
- The probe does not **call** any resolved symbol. Resolving an address is
  safe; calling a JSC function with the wrong arguments would crash Bun.
