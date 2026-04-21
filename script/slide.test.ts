// verifies the ASLR slide and resolve() using the one JSC symbol bun exports.
// if verifyAnchor() passes, resolve() is trustworthy for any offset from a dSYM.
// run: make script/slide.node 2>&1 && bun test script/slide.test.ts
import { test, expect, describe } from "bun:test";
import { createRequire } from "module";

const require = createRequire(import.meta.url);
const slide = require("./slide.node") as {
   slide(): bigint | null;
   imageBase(): bigint | null;
   resolve(offset: number | bigint): bigint | null;
   verifyAnchor(): {
      offset: bigint;
      resolved: bigint;
      dlsym: bigint | null;
      match: boolean;
   };
};

describe("slide addon", () => {
   test("returns non-null slide and imageBase", () => {
      const s = slide.slide();
      const ib = slide.imageBase();
      expect(s).not.toBeNull();
      expect(ib).not.toBeNull();
      console.log("slide:    ", `0x${s!.toString(16)}`);
      console.log("imageBase:", `0x${ib!.toString(16)}`);
   });

   test("resolve(offset) matches dlsym — anchor verification", () => {
      const v = slide.verifyAnchor();
      console.log("anchor offset:  ", `0x${v.offset.toString(16)}`);
      console.log("resolve() gave: ", `0x${v.resolved.toString(16)}`);
      console.log(
         "dlsym() gave:   ",
         v.dlsym ? `0x${v.dlsym.toString(16)}` : "null",
      );
      console.log("match:          ", v.match);
      expect(v.dlsym).not.toBeNull();
      expect(v.match).toBe(true);
   });

   test("resolve() is consistent across calls", () => {
      expect(slide.resolve(0x01f13be0n)).toBe(slide.resolve(0x01f13be0n));
   });

   test("imageBase + offset == resolve(offset)", () => {
      const offset = 0x01f13be0n;
      expect(slide.imageBase()! + offset).toBe(slide.resolve(offset));
   });
});

describe("hypothetical SamplingProfiler offsets", () => {
   // replace with values from: dwarfdump -lookup <sym> path/to/bun.dSYM
   // or from a debug build's symbol table (nm -a bun.debug)
   const STRIPPED_CANDIDATES: Record<string, bigint> = {
      // "SamplingProfiler::start": 0x???n,
      // "VM::current":             0x???n,
   };

   test("prints what addresses stripped symbols would have if offsets are known", () => {
      if (Object.keys(STRIPPED_CANDIDATES).length === 0) {
         console.log(
            "no candidate offsets yet — extract from bun.dSYM and add here",
         );
         return;
      }
      for (const [name, offset] of Object.entries(STRIPPED_CANDIDATES)) {
         console.log(`${name}: 0x${slide.resolve(offset)?.toString(16)}`);
      }
   });
});
