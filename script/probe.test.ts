import { test, expect } from "bun:test";
import { createRequire } from "module";

const require = createRequire(import.meta.url);

// load both link strategies and compare results
const lazy   = require("./probe_lazy.node");
const direct = require("./probe_direct.node");

function runProbe(label: string, addon: { probe: () => unknown }) {
  console.log(`\n=== ${label} ===`);
  const result = addon.probe();
  console.log(JSON.stringify(result, null, 2));
  return result as {
    jsc_symbols: Record<string, number | null>;
    host: { host_image: string | null; host_base: number | null; main_handle_ok: boolean };
  };
}

test("probe_lazy loads and runs", () => {
  const r = runProbe("probe_lazy.node (-undefined dynamic_lookup)", lazy);
  expect(r).toHaveProperty("jsc_symbols");
  expect(r).toHaveProperty("host");
  // we must be running inside bun
  expect(r.host.host_image).toMatch(/bun/i);
  expect(r.host.main_handle_ok).toBe(true);
});

test("probe_direct loads and runs", () => {
  const r = runProbe("probe_direct.node (-bundle_loader bun)", direct);
  expect(r).toHaveProperty("jsc_symbols");
  expect(r.host.host_image).toMatch(/bun/i);
});

test("both strategies agree on symbol presence", () => {
  const l = lazy.probe()   as { jsc_symbols: Record<string, unknown> };
  const d = direct.probe() as { jsc_symbols: Record<string, unknown> };
  for (const sym of Object.keys(l.jsc_symbols)) {
    const lazyNull   = l.jsc_symbols[sym] === null;
    const directNull = d.jsc_symbols[sym] === null;
    expect(lazyNull).toBe(directNull);
  }
});
