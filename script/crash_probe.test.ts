// run: make script/crash_probe.node && bun test script/crash_probe.test.ts
//
// probes samplesAsJSON() with a SIGSEGV handler to identify the crashing instruction.
// if crashed=true, faultPcOffset is the offset into the bun binary that faulted —
// feed that to: xcrun objdump -d --start-address=<VA> --stop-address=<VA+64> bun

import { test, expect, describe } from "bun:test";
import { createRequire } from "module";

const require = createRequire(import.meta.url);

const probe = require("./crash_probe.node") as {
  probeSamplesAsJSON(): {
    imageBase:     bigint;
    profilerAddr:  bigint | null;
    crashed:       boolean;
    implNull?:     boolean;
    faultAddr?:    bigint;
    faultPc?:      bigint;
    faultPcOffset?: bigint;
    error?:        string;
  };
};

const sampler = require("./sampler.node") as {
  enable():  void;
  disable(): void;
};

describe("crash_probe", () => {
  test("probeSamplesAsJSON — captures crash or returns result", () => {
    sampler.enable();
    // bigger workload to ensure the profiler captures samples
    let x = 0;
    for (let i = 0; i < 5_000_000; i++) x += Math.sqrt(i);
    // call takeSamplesAsJSON while profiler is still running, before disable
    const r = probe.probeSamplesAsJSON();
    sampler.disable();

    console.log("imageBase:    ", `0x${r.imageBase.toString(16)}`);
    console.log("profilerAddr: ", r.profilerAddr ? `0x${r.profilerAddr.toString(16)}` : "null");
    console.log("crashed:      ", r.crashed);

    if (r.error) {
      console.log("error:", r.error);
    }

    if (r.crashed) {
      console.log("faultAddr:    ", r.faultAddr ? `0x${r.faultAddr.toString(16)}` : "null");
      console.log("faultPc:      ", r.faultPc   ? `0x${r.faultPc.toString(16)}`   : "null");
      console.log("faultPcOffset:", r.faultPcOffset ? `0x${r.faultPcOffset.toString(16)}` : "null");

      // print the objdump command to inspect the crashing instruction
      if (r.faultPcOffset) {
        const base = BigInt("0x100000000");
        const va   = base + r.faultPcOffset;
        const vaEnd = va + 64n;
        console.log(
          "\ndisassemble with:\n" +
          `  xcrun objdump -d --start-address=0x${va.toString(16)} ` +
          `--stop-address=0x${vaEnd.toString(16)} $(which bun)`
        );
      }
    } else {
      console.log("implNull:     ", r.implNull);
      console.log("hexdump:      ", (r as any).hexdump);
      const json = (r as any).json as string;
      console.log("json length:  ", json.length);
      if (json.length > 0) console.log("preview:      ", json.slice(0, 300));
      if (json.length > 0) {
        const parsed = JSON.parse(json);
        console.log("top-level keys:", Object.keys(parsed));
      }
    }

    expect(r).toHaveProperty("imageBase");
    expect(r).toHaveProperty("crashed");
    expect(r.imageBase).toBeGreaterThan(0n);

    if (!r.crashed) {
      const json = (r as any).json as string;
      expect(json.length).toBeGreaterThan(0);
      expect(() => JSON.parse(json)).not.toThrow();
    }
  });
});
