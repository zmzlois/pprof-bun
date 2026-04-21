// probe.cc — N-API addon that hunts for JSC symbols inside the host process.
//
// two link strategies are tested from the Makefile:
//   probe_lazy.node  — -undefined dynamic_lookup (all unresolved deferred to host)
//   probe_direct.node — -bundle_loader $(which bun)  (linker validates against bun)
//
// dladdr reverse-lookup is included so the probe has something meaningful to
// report even when the binary is stripped: we walk from a known napi symbol
// address back to the image that owns it, confirming we're inside bun's
// address space.

#include <napi.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>

// jsc mangled symbols to hunt for — adjust as you grep vendored headers
static const char* kCandidates[] = {
  "_ZN3JSC15SamplingProfiler5startEv",    // SamplingProfiler::start()
  "_ZN3JSC2VM7currentEv",                  // VM::current()
  "_ZN3JSC11HeapProfilerC1ERNS_2VME",     // HeapProfiler::HeapProfiler(VM&)
  "_ZN3JSC9CallFrame13describeFrameEv",   // the one symbol bun actually exports
  nullptr,
};

// walk a known napi symbol back to its owning image via dladdr
static Napi::Object ProbeHost(Napi::Env env) {
  auto result = Napi::Object::New(env);

  // napi_create_object is guaranteed exported by any napi host — use it as
  // an anchor to identify which image we're actually running inside
  void* anchor = reinterpret_cast<void*>(&napi_create_object);
  Dl_info info{};
  if (dladdr(anchor, &info) && info.dli_fname) {
    result.Set("host_image", Napi::String::New(env, info.dli_fname));
    result.Set("host_base",  Napi::Number::New(env, reinterpret_cast<uintptr_t>(info.dli_fbase)));
  } else {
    result.Set("host_image", env.Null());
    result.Set("host_base",  env.Null());
  }

  // confirm RTLD_DEFAULT handle opens
  void* main_handle = dlopen(nullptr, RTLD_LAZY | RTLD_NOLOAD);
  result.Set("main_handle_ok", Napi::Boolean::New(env, main_handle != nullptr));

  return result;
}

Napi::Value Probe(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  auto result = Napi::Object::New(env);

  // scan for jsc symbols in the host process
  auto symbols = Napi::Object::New(env);
  for (int i = 0; kCandidates[i]; i++) {
    void* p = dlsym(RTLD_DEFAULT, kCandidates[i]);
    if (p) {
      // symbol found — record its virtual address
      symbols.Set(kCandidates[i], Napi::Number::New(env, reinterpret_cast<uintptr_t>(p)));
    } else {
      symbols.Set(kCandidates[i], env.Null());
    }
  }
  result.Set("jsc_symbols", symbols);

  // host image probe via dladdr
  result.Set("host", ProbeHost(env));

  return result;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("probe", Napi::Function::New(env, Probe));
  return exports;
}

NODE_API_MODULE(probe, Init)
