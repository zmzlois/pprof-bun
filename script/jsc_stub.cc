// src/candidates/jsc_stub.cc
//
// This file is NEVER linked into a real program. It exists solely to be
// compiled to an object file whose undefined symbol table we read to recover
// canonical Itanium-mangled names for JSC functions we might want to probe.
//
// How it works:
//   clang++ -c -std=c++17 src/candidates/jsc_stub.cc -o stub.o
//   nm stub.o | awk '/ U / {print $NF}' | c++filt
//
// The mangled name produced depends on:
//   1. the namespace path (JSC::...)
//   2. the function name
//   3. the exact parameter and return types
//
// Signatures below are best-effort forward declarations based on public
// WebKit source. If real JSC has slightly different signatures (refs vs
// pointers, extra overloads, extra parameters), the mangled names will
// NOT match Bun's binary and extract-jsc-symbols.sh is the authoritative
// source instead.
//
// Treat this file as a fallback for the fully-stripped-binary case, and
// update signatures here as you confirm them against WebKit upstream.

namespace WTF {
// Forward-decl only; we never dereference.
template <typename T> class Ref;
template <typename T> class RefPtr;
class String;
class PrintStream;
}

namespace JSC {

class VM;
class ExecState;
class CallFrame;
class JSGlobalObject;
class JSValue;
class JSLockHolder;

// VM — the JS heap/isolate equivalent. Key question: how to obtain one.
class VM {
 public:
  // This isn't a real JSC API, but is the shape of what we'd want.
  // Real JSC doesn't have VM::current(); you typically obtain VM through
  // a JSGlobalObject. Probing this anyway tells us if Bun added a helper.
  static VM& current();
  static VM& sharedInstance();
};

// SamplingProfiler — the CPU profiler. Public enough to be worth probing.
class SamplingProfiler {
 public:
  SamplingProfiler(VM&, double /*sampleInterval*/);
  ~SamplingProfiler();
  void start();
  void startWithFrequency(unsigned /*hz*/);
  void stop();
  void clearData();
  void noticeCurrentThreadAsJSCExecutionThread();
  void setTimingInterval(double);
};

// HeapProfiler — allocation profiling. Harder to get at than the sampling one.
class HeapProfiler {
 public:
  explicit HeapProfiler(VM&);
  ~HeapProfiler();
};

// HeapSnapshotBuilder — used for point-in-time snapshots.
class HeapSnapshotBuilder {
 public:
  explicit HeapSnapshotBuilder(HeapProfiler&);
  ~HeapSnapshotBuilder();
  void buildSnapshot();
};

}  // namespace JSC

// Force the compiler to emit undefined references to every function we want
// to probe for. The __anchor symbol itself is irrelevant; we only read the
// "U" (undefined) entries from nm.
extern "C" void __pprof_bun_anchor() {
  JSC::VM& vm1 = JSC::VM::current();
  JSC::VM& vm2 = JSC::VM::sharedInstance();
  (void)vm1; (void)vm2;

  JSC::SamplingProfiler sp(vm1, 0.001);
  sp.start();
  sp.startWithFrequency(1000);
  sp.stop();
  sp.clearData();
  sp.noticeCurrentThreadAsJSCExecutionThread();
  sp.setTimingInterval(0.001);

  JSC::HeapProfiler hp(vm1);
  JSC::HeapSnapshotBuilder hsb(hp);
  hsb.buildSnapshot();
}
