// run: make script/sampler.node && bun test script/sampler.test.ts
//
// progression:
//   1. vmAddress()     — confirms we can walk napi_env → GlobalObject → VM
//   2. enable/disable  — confirms void calls into the profiler work without a crash
//   3. samplesAsJSON() — confirms we can read the WTF::String result back

import { test, expect, describe } from "bun:test";
import { createRequire } from "module";

const require = createRequire(import.meta.url);
const sampler = require("./sampler.node") as {
  vmAddress():       bigint | null;
  profilerAddress(): bigint | null;
  enable():          void;
  disable():         void;
  samplesAsJSON():   string;
};

describe("sampler", () => {
  test("vmAddress() resolves a non-zero pointer", () => {
    const addr = sampler.vmAddress();
    console.log("VM pointer:", addr ? `0x${addr.toString(16)}` : "null");
    expect(addr).not.toBeNull();
    expect(addr!).toBeGreaterThan(0n);
  });

  test("enable() and disable() do not crash", () => {
    expect(() => {
      sampler.enable();
      sampler.disable();
    }).not.toThrow();
  });

  test("profilerAddress() is non-null after enable()", () => {
    sampler.enable();
    const addr = sampler.profilerAddress();
    console.log("SamplingProfiler*:", addr ? `0x${addr.toString(16)}` : "null");
    expect(addr).not.toBeNull();
    sampler.disable();
  });

  test("samplesAsJSON() without disable — profiler still running", () => {
    sampler.enable();
    let x = 0;
    for (let i = 0; i < 5_000_000; i++) x += Math.sqrt(i);
    // intentionally NOT calling disable() — test if we can read while profiler runs
    const json = sampler.samplesAsJSON();
    console.log("json length (no disable):", json.length);
    if (json.length > 0) console.log("preview:", json.slice(0, 200));
    sampler.disable(); // clean up after
  });

  test("samplesAsJSON() returns valid JSON after profiling a workload", () => {
    sampler.enable();
    const profilerAddr = sampler.profilerAddress();
    console.log("profiler before workload:", profilerAddr ? `0x${profilerAddr.toString(16)}` : "null");

    let x = 0;
    for (let i = 0; i < 5_000_000; i++) x += Math.sqrt(i);

    sampler.disable();
    const profilerAfter = sampler.profilerAddress();
    console.log("profiler after disable:", profilerAfter ? `0x${profilerAfter.toString(16)}` : "null");

    if (!profilerAfter) {
      console.log("profiler was destroyed by disable() — skipping samplesAsJSON");
      return;
    }

    const json = sampler.samplesAsJSON();
    console.log("raw json length:", json.length);
    console.log("preview:", json.slice(0, 200));

    expect(json.length).toBeGreaterThan(0);
    expect(() => JSON.parse(json)).not.toThrow();
    const parsed = JSON.parse(json);
    console.log("top-level keys:", Object.keys(parsed));
  });
});
