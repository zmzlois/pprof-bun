// sampler.cc — calls JSC's built-in SamplingProfiler through bun's stripped binary.
//
// call chain:
//   napi_env
//     → NapiEnv__globalObject(env)              → JSGlobalObject*
//     → JSC__JSGlobalObject__vm(global)          → VM*
//     → VM::ensureSamplingProfiler(vm, &fakeRef) → SamplingProfiler*
//     → SamplingProfiler::start(profiler)
//     → ... user code runs ...
//     → VM::disableSamplingProfiler(vm)
//     → VM::takeSamplingProfilerSamplesAsJSON(vm) → WTF::String (via x8)
//
// all offsets extracted from bun-darwin-aarch64-profile binary (same layout as
// production, verified via the CallFrame::describeFrame anchor: 0x01F13BE0 in both).
//
// WTF::String ABI (arm64 AAPCS64):
//   non-trivially-destructible type → returned via hidden x8 pointer.
//   we fake this by declaring WTFString with an empty user destructor, which
//   makes the compiler apply x8 convention automatically.
//
// WTF::Stopwatch ABI:
//   Stopwatch::create() is inlined everywhere (not exported). we construct a
//   fake one on the heap that matches the RefCounted<Stopwatch> memory layout.
//   ensureSamplingProfiler takes Ref<Stopwatch>&& — an rvalue ref, passed as
//   a pointer to our FakeRef struct on the stack (AAPCS64 ref convention).

#include <napi.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstring>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

// ─── binary offsets ───────────────────────────────────────────────────────────
// offset = symbol_addr_in_profile_binary - 0x100000000

static constexpr uint64_t kNapiEnvGlobalObject  = 0x001193ad4; // NapiEnv__globalObject(env)
static constexpr uint64_t kGlobalObjectVM        = 0x00100205c; // JSC__JSGlobalObject__vm(global)
static constexpr uint64_t kVMEnsureSampling      = 0x00262f7f4; // VM::ensureSamplingProfiler(VM&, Ref<Stopwatch>&&)
static constexpr uint64_t kSamplingProfilerStart = 0x0025b73c0; // SamplingProfiler::start()
static constexpr uint64_t kVMDisableSampling     = 0x00263117c; // VM::disableSamplingProfiler()
static constexpr uint64_t kVMTakeSamplesAsJSON   = 0x002631334; // VM::takeSamplingProfilerSamplesAsJSON()
// kVMTakeSamplesAsJSON returns a raw JSON::Object* via x8 ABI, not a WTF::String.
// this function converts the JSON::Object* to a real WTF::String via a StringBuilder.
// identified by disassembling callers of stackTracesAsJSON at 0x10110ea74-0x10110ea7c.
static constexpr uint64_t kJSONBuilderToString   = 0x014c835c;

// ─── WTF::Stopwatch shim ─────────────────────────────────────────────────────
// WTF::RefCounted<T> base: just { unsigned m_refCount; } in release builds.
// WTF::Stopwatch fields (from WebKit source):
//   double m_elapsedTime     (+8, after 4-byte m_refCount + 4 padding)
//   MonotonicTime m_lastStartTime  (+16, MonotonicTime wraps a double)
//   bool m_isActive          (+24)
// total: ~32 bytes. we allocate via new so RefCounted::deref() can delete it.
struct alignas(8) FakeStopwatch {
  unsigned m_refCount    = 1;   // RefCounted base — must start at 1
  unsigned _pad          = 0;
  double m_elapsedTime   = 0.0;
  double m_lastStartTime = 0.0;
  bool m_isActive        = false;
  uint8_t _pad2[7]       = {};
};

// WTF::Ref<T> is a non-null smart pointer — layout is just a T* in a struct.
// ensureSamplingProfiler takes Ref<Stopwatch>&&, which the compiler passes
// as a pointer to this struct (rvalue ref ABI on arm64).
struct FakeRef {
  FakeStopwatch* ptr;
};

// ─── WTF::String shim ────────────────────────────────────────────────────────
// WTF::String = RefPtr<StringImpl> = one pointer (8 bytes).
// empty user destructor → compiler treats as non-trivially-destructible
// → x8 hidden-return-pointer convention applied automatically on arm64.
struct WTFString {
  void* impl;
  ~WTFString() {} // triggers x8 ABI — do not remove
};

// Leaf StringImpl layout (verified via disassembly of 0x1015278b8 in bun):
//   +0  unsigned m_refCount
//   +4  unsigned m_length
//   +8  const LChar* / UChar*  (8-byte data pointer)
//   +16 unsigned m_hashAndFlags  (bit 2 = is8Bit)
static constexpr uint32_t kIs8BitFlag = 1u << 2;

static std::string ReadWTFString(void* impl) {
  if (!impl) return "";
  auto* b = reinterpret_cast<const uint8_t*>(impl);
  uint32_t length      = *reinterpret_cast<const uint32_t*>(b + 4);
  const void* data_ptr = *reinterpret_cast<const void* const*>(b + 8);
  uint32_t hashAndFlags = *reinterpret_cast<const uint32_t*>(b + 16);
  if (!length || length > 64 * 1024 * 1024 || !data_ptr) return "";
  if (hashAndFlags & kIs8BitFlag) {
    return std::string(reinterpret_cast<const char*>(data_ptr), length);
  }
  // 16-bit UChar: JSON output is ASCII so masking to 7-bit is safe
  const uint16_t* data = reinterpret_cast<const uint16_t*>(data_ptr);
  std::string out(length, '\0');
  for (uint32_t i = 0; i < length; i++) out[i] = static_cast<char>(data[i] & 0x7f);
  return out;
}

// ─── image base ──────────────────────────────────────────────────────────────

static uintptr_t BunImageBase() {
  static uintptr_t base = 0;
  if (base) return base;
  Dl_info info{};
  dladdr(reinterpret_cast<void*>(&napi_create_object), &info);
  base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  return base;
}

static void* Resolve(uint64_t offset) {
  return reinterpret_cast<void*>(BunImageBase() + offset);
}

// ─── call chain ──────────────────────────────────────────────────────────────

static void* GetGlobalObject(napi_env env) {
  using Fn = void*(void*);
  return reinterpret_cast<Fn*>(Resolve(kNapiEnvGlobalObject))(env);
}

static void* GetVM(void* global) {
  using Fn = void*(void*);
  return reinterpret_cast<Fn*>(Resolve(kGlobalObjectVM))(global);
}

// create a heap Stopwatch, wrap in FakeRef, call ensureSamplingProfiler,
// then immediately start the profiler. returns the SamplingProfiler* or null.
static void* EnsureAndStart(void* vm) {
  FakeStopwatch* sw = new FakeStopwatch();
  FakeRef ref{sw};

  // ensureSamplingProfiler(VM&, Ref<Stopwatch>&&) → SamplingProfiler&
  // arm64: second arg is pointer to FakeRef (rvalue ref convention)
  // return: SamplingProfiler* in x0 (reference return = pointer)
  using EnsureFn = void*(void*, FakeRef*);
  void* profiler = reinterpret_cast<EnsureFn*>(Resolve(kVMEnsureSampling))(vm, &ref);
  if (!profiler) return nullptr;

  // SamplingProfiler::start() — void(this)
  using StartFn = void(void*);
  reinterpret_cast<StartFn*>(Resolve(kSamplingProfilerStart))(profiler);
  return profiler;
}

// offset of m_samplingProfiler within VM struct — read from disassembly of
// takeSamplingProfilerSamplesAsJSON: mov w8, #0x60b8 / movk w8, #0x2, lsl #16
static constexpr uint64_t kVMSamplingProfilerOffset = 0x260b8;

// ─── exported JS functions ───────────────────────────────────────────────────

// profilerAddress() → BigInt | null — reads vm->m_samplingProfiler directly
// lets us confirm the profiler was created before calling samplesAsJSON
Napi::Value ProfilerAddress(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  void* vm = GetVM(GetGlobalObject(env));
  if (!vm) return env.Null();
  void* profiler = *reinterpret_cast<void**>(
    reinterpret_cast<uint8_t*>(vm) + kVMSamplingProfilerOffset
  );
  if (!profiler) return env.Null();
  return Napi::BigInt::New(env, reinterpret_cast<uint64_t>(profiler));
}

// vmAddress() → BigInt
Napi::Value VmAddress(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  void* global = GetGlobalObject(env);
  if (!global) return env.Null();
  void* vm = GetVM(global);
  if (!vm) return env.Null();
  return Napi::BigInt::New(env, reinterpret_cast<uint64_t>(vm));
}

// enable() — creates the profiler (if not already created) and starts it
Napi::Value Enable(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  void* vm = GetVM(GetGlobalObject(env));
  if (!vm) {
    Napi::Error::New(env, "could not resolve VM pointer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  void* profiler = EnsureAndStart(vm);
  if (!profiler) {
    Napi::Error::New(env, "ensureSamplingProfiler returned null").ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

// disable() — stops the sampling profiler
Napi::Value Disable(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  void* vm = GetVM(GetGlobalObject(env));
  if (!vm) {
    Napi::Error::New(env, "could not resolve VM pointer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  using Fn = void(void*);
  reinterpret_cast<Fn*>(Resolve(kVMDisableSampling))(vm);
  return env.Undefined();
}

// samplesAsJSON() → string
Napi::Value SamplesAsJSON(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  void* vm = GetVM(GetGlobalObject(env));
  if (!vm) {
    Napi::Error::New(env, "could not resolve VM pointer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // step 1: get the JSON::Object* (raw builder) from the profiler
  using GetFn = WTFString(void*);
  WTFString builder = reinterpret_cast<GetFn*>(Resolve(kVMTakeSamplesAsJSON))(vm);
  if (!builder.impl) return Napi::String::New(env, "");
  // step 2: serialize the JSON::Object* to a real WTF::String (leaf StringImpl)
  using ToStrFn = WTFString(void*);
  WTFString str = reinterpret_cast<ToStrFn*>(Resolve(kJSONBuilderToString))(builder.impl);
  return Napi::String::New(env, ReadWTFString(str.impl));
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("vmAddress",       Napi::Function::New(env, VmAddress));
  exports.Set("profilerAddress", Napi::Function::New(env, ProfilerAddress));
  exports.Set("enable",          Napi::Function::New(env, Enable));
  exports.Set("disable",         Napi::Function::New(env, Disable));
  exports.Set("samplesAsJSON",   Napi::Function::New(env, SamplesAsJSON));
  return exports;
}

NODE_API_MODULE(sampler, Init)
