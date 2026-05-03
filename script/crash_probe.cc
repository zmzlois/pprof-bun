// crash_probe.cc — wraps samplesAsJSON() with a SIGSEGV handler so we can
// report the faulting address and instruction pointer without bun aborting.

#include <napi.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <signal.h>
#include <setjmp.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
// arm64 thread state for reading __ss.__pc from ucontext
#include <mach/mach.h>
#include <sys/ucontext.h>

// ─── offsets (same as sampler.cc) ────────────────────────────────────────────
static constexpr uint64_t kNapiEnvGlobalObject    = 0x001193ad4;
static constexpr uint64_t kGlobalObjectVM          = 0x00100205c;
static constexpr uint64_t kVMTakeSamplesAsJSON     = 0x002631334;
static constexpr uint64_t kVMSamplingProfilerOff   = 0x260b8;
// found by disassembling VM::disableSamplingProfiler: bl at +0x48 with x0=profiler
// (was wrong before due to 10-digit vs 9-digit VA — off by one hex zero)
static constexpr uint64_t kSamplingProfilerStop    = 0x025b7458;
// kVMTakeSamplesAsJSON returns a JSON::Object* (the raw builder), NOT a WTF::String.
// this function serializes it to a real WTF::String via a StringBuilder internally.
// confirmed by disassembly: 0x10110ea74-0x10110ea7c in the callers of stackTracesAsJSON.
static constexpr uint64_t kJSONBuilderToString     = 0x014c835c;

// x8 return ABI: empty user destructor makes the compiler use hidden x8 pointer
struct WTFString { void* impl; ~WTFString() {} };

// Leaf StringImpl layout (verified via disassembly of 0x1015278b8 in bun):
//   +0  unsigned m_refCount
//   +4  unsigned m_length
//   +8  const LChar* / UChar*  (8-byte data pointer)
//   +16 unsigned m_hashAndFlags (bit 2 = is8Bit)
//
// is8Bit flag in leaf m_hashAndFlags: bit 2
static constexpr uint32_t kIs8BitFlag = 1u << 2;

// valid pointer heuristic: user-space arm64 (0x100000000 .. 0x800000000000)
static bool IsLikelyPtr(const void* p) {
  auto v = reinterpret_cast<uintptr_t>(p);
  return v >= 0x100000000ULL && v < 0x800000000000ULL;
}

// reads one leaf StringImpl (data_ptr at +8, hashAndFlags at +16)
static std::string ReadLeafStringImpl(const uint8_t* b, uint32_t length) {
  const void* data_ptr = *reinterpret_cast<const void* const*>(b + 8);
  if (!data_ptr || !IsLikelyPtr(data_ptr)) return "";
  uint32_t hashAndFlags = *reinterpret_cast<const uint32_t*>(b + 16);
  if (hashAndFlags & kIs8BitFlag) {
    return std::string(reinterpret_cast<const char*>(data_ptr), length);
  }
  // 16-bit UChar: JSON is ASCII so mask to 7-bit
  const uint16_t* data = reinterpret_cast<const uint16_t*>(data_ptr);
  std::string out(length, '\0');
  for (uint32_t i = 0; i < length; i++) out[i] = static_cast<char>(data[i] & 0x7f);
  return out;
}

static std::string ReadWTFString(void* impl) {
  if (!impl) return "";
  auto* b = reinterpret_cast<const uint8_t*>(impl);
  uint32_t length = *reinterpret_cast<const uint32_t*>(b + 4);
  if (!length || length > 64 * 1024 * 1024) return "";
  return ReadLeafStringImpl(b, length);
}

static uintptr_t BunImageBase() {
  static uintptr_t base = 0;
  if (base) return base;
  Dl_info info{};
  dladdr(reinterpret_cast<void*>(&napi_create_object), &info);
  base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  return base;
}

static void* Resolve(uint64_t off) {
  return reinterpret_cast<void*>(BunImageBase() + off);
}

static void* GetGlobalObject(napi_env env) {
  using Fn = void*(void*);
  return reinterpret_cast<Fn*>(Resolve(kNapiEnvGlobalObject))(env);
}

static void* GetVM(void* global) {
  using Fn = void*(void*);
  return reinterpret_cast<Fn*>(Resolve(kGlobalObjectVM))(global);
}

// ─── SIGSEGV catch ───────────────────────────────────────────────────────────

static sigjmp_buf g_jmp;
static uintptr_t  g_fault_addr = 0;
static uintptr_t  g_fault_pc   = 0;

static void SigHandler(int /*sig*/, siginfo_t* info, void* ctx) {
  g_fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
  auto* uc = static_cast<ucontext_t*>(ctx);
  g_fault_pc = static_cast<uintptr_t>(uc->uc_mcontext->__ss.__pc);
  siglongjmp(g_jmp, 1);
}

// probeSamplesAsJSON() → { imageBase, profilerAddr, crashed, implNull?,
//                          hexdump?, json?, faultAddr?, faultPc?, faultPcOffset? }
Napi::Value ProbeSamplesAsJSON(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  void* vm = GetVM(GetGlobalObject(env));
  auto result = Napi::Object::New(env);

  uintptr_t base = BunImageBase();
  result.Set("imageBase", Napi::BigInt::New(env, static_cast<uint64_t>(base)));

  if (!vm) {
    result.Set("error", Napi::String::New(env, "null vm"));
    return result;
  }

  void* profiler = *reinterpret_cast<void**>(
    reinterpret_cast<uint8_t*>(vm) + kVMSamplingProfilerOff
  );
  result.Set("profilerAddr", profiler
    ? Napi::Value(Napi::BigInt::New(env, reinterpret_cast<uint64_t>(profiler)))
    : Napi::Value(env.Null()));

  // install SIGSEGV handler — keep it active until we finish reading the string
  struct sigaction sa{}, old{};
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sa.sa_sigaction = SigHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &old);

  g_fault_addr = 0;
  g_fault_pc   = 0;

  if (sigsetjmp(g_jmp, 1) == 0) {
    // stop the profiler background thread before reading JSON to avoid race
    // where some rope fiber nodes are allocated but not yet filled in
    if (profiler) {
      using StopFn = void(void*);
      reinterpret_cast<StopFn*>(Resolve(kSamplingProfilerStop))(profiler);
    }

    // kVMTakeSamplesAsJSON returns a JSON::Object* (not a WTF::String) via x8 ABI.
    // we must call kJSONBuilderToString to convert it to a real WTF::String.
    using GetFn = WTFString(void*);
    WTFString builder = reinterpret_cast<GetFn*>(Resolve(kVMTakeSamplesAsJSON))(vm);

    bool impl_null = (builder.impl == nullptr);
    std::string hexdump;
    std::string json_str;

    if (!impl_null) {
      // serialize the JSON::Object* to a real WTF::String (leaf StringImpl)
      using ToStrFn = WTFString(void*);
      WTFString str = reinterpret_cast<ToStrFn*>(Resolve(kJSONBuilderToString))(builder.impl);

      if (str.impl) {
        auto* sb = reinterpret_cast<const uint8_t*>(str.impl);
        uint32_t slen = *reinterpret_cast<const uint32_t*>(sb + 4);
        char dbuf[256];
        std::snprintf(dbuf, sizeof(dbuf),
          "builder=0x%llx str_impl=0x%llx str_len=%u",
          (unsigned long long)(uintptr_t)builder.impl,
          (unsigned long long)(uintptr_t)str.impl, slen);
        hexdump = dbuf;
        json_str = ReadWTFString(str.impl);
      } else {
        hexdump = "kJSONBuilderToString returned null impl";
      }
    }

    // only restore bun's handler after we're done reading string memory
    sigaction(SIGSEGV, &old, nullptr);

    result.Set("crashed",   Napi::Boolean::New(env, false));
    result.Set("implNull",  Napi::Boolean::New(env, impl_null));
    result.Set("hexdump",   Napi::String::New(env, hexdump));
    result.Set("json",      Napi::String::New(env, json_str));
    result.Set("faultAddr", env.Null());
    result.Set("faultPc",   env.Null());
  } else {
    sigaction(SIGSEGV, &old, nullptr);
    result.Set("crashed",   Napi::Boolean::New(env, true));
    result.Set("faultAddr", Napi::BigInt::New(env, static_cast<uint64_t>(g_fault_addr)));
    result.Set("faultPc",   Napi::BigInt::New(env, static_cast<uint64_t>(g_fault_pc)));
    uintptr_t pc_offset = g_fault_pc - base;
    result.Set("faultPcOffset", Napi::BigInt::New(env, static_cast<uint64_t>(pc_offset)));
  }

  return result;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("probeSamplesAsJSON", Napi::Function::New(env, ProbeSamplesAsJSON));
  return exports;
}

NODE_API_MODULE(crash_probe, Init)
