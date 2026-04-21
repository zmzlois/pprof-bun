// Address Space layout Randomisation
// slide.cc — exposes the bun image's ASLR slide so JS can map binary offsets
// to live virtual addresses without recompiling.
//
// three exports:
//   slide()            → BigInt  ASLR slide (runtime_base - file_vmaddr)
//   imageBase()        → BigInt  runtime base address of the bun image
//   resolve(offset)    → BigInt  runtime VA for a given binary offset
//
// offsets come from the export trie or a dSYM:
//   dyld_info -exports $(which bun)      — for exported symbols
//   dsymutil / dwarfdump                 — for stripped symbols from a debug build
//
// verification anchor: the one JSC symbol bun still exports —
//   _ZN3JSC9CallFrame13describeFrameEv  offset 0x01F13BE0
// resolve(0x01F13BE0) should equal dlsym(RTLD_DEFAULT, that symbol).

#include <napi.h>
#include <dlfcn.h>
#include <cstdint>
#include <mach-o/dyld.h>   // _dyld_get_image_vmaddr_slide, _dyld_get_image_header
#include <mach-o/loader.h> // mach_header_64

// find the load index of the image that owns a given address
static int32_t FindImageIndex(const void* addr) {
  Dl_info info{};
  if (!dladdr(addr, &info)) return -1;

  uint32_t count = _dyld_image_count();
  for (uint32_t i = 0; i < count; i++) {
    if (_dyld_get_image_header(i) ==
        reinterpret_cast<const mach_header*>(info.dli_fbase)) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// anchor: napi_create_object is guaranteed exported by any napi host
static int32_t BunImageIndex() {
  static int32_t idx = FindImageIndex(reinterpret_cast<void*>(&napi_create_object));
  return idx;
}

// ─── exported JS functions ────────────────────────────────────────────────────

// slide() → BigInt
Napi::Value Slide(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  int32_t idx = BunImageIndex();
  if (idx < 0) return env.Null();
  intptr_t slide = _dyld_get_image_vmaddr_slide(static_cast<uint32_t>(idx));
  return Napi::BigInt::New(env, static_cast<int64_t>(slide));
}

// imageBase() → BigInt
Napi::Value ImageBase(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  int32_t idx = BunImageIndex();
  if (idx < 0) return env.Null();
  const void* hdr = _dyld_get_image_header(static_cast<uint32_t>(idx));
  return Napi::BigInt::New(env, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hdr)));
}

// resolve(offset: number | bigint) → BigInt
// offset is the value from `dyld_info -exports` or a dSYM, relative to image base
Napi::Value Resolve(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "resolve(offset) requires one argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  uint64_t offset = 0;
  if (info[0].IsBigInt()) {
    bool lossless = false;
    offset = info[0].As<Napi::BigInt>().Uint64Value(&lossless);
  } else if (info[0].IsNumber()) {
    offset = static_cast<uint64_t>(info[0].As<Napi::Number>().DoubleValue());
  } else {
    Napi::TypeError::New(env, "offset must be a number or bigint").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  int32_t idx = BunImageIndex();
  if (idx < 0) return env.Null();
  const void* hdr = _dyld_get_image_header(static_cast<uint32_t>(idx));
  uint64_t va = reinterpret_cast<uintptr_t>(hdr) + offset;
  return Napi::BigInt::New(env, va);
}

// verifyAnchor() → { offset, resolved, dlsym, match }
// cross-checks resolve(CALFRAME_OFFSET) against dlsym in one C++ call,
// so the test doesn't need bun:ffi to open a null handle.
static const char kAnchorSym[]    = "_ZN3JSC9CallFrame13describeFrameEv";
static const uint64_t kAnchorOffset = 0x01F13BE0ULL;

Napi::Value VerifyAnchor(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  auto result = Napi::Object::New(env);

  int32_t idx = BunImageIndex();
  const void* hdr = (idx >= 0) ? _dyld_get_image_header(static_cast<uint32_t>(idx)) : nullptr;
  uint64_t resolved = hdr ? (reinterpret_cast<uintptr_t>(hdr) + kAnchorOffset) : 0;

  void* sym = dlsym(RTLD_DEFAULT, kAnchorSym);
  uint64_t dlsym_va = reinterpret_cast<uintptr_t>(sym);

  result.Set("offset",   Napi::BigInt::New(env, kAnchorOffset));
  result.Set("resolved", Napi::BigInt::New(env, resolved));
  result.Set("dlsym",    sym ? Napi::Value(Napi::BigInt::New(env, dlsym_va))
                              : Napi::Value(env.Null()));
  result.Set("match",    Napi::Boolean::New(env, sym && resolved == dlsym_va));
  return result;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("slide",        Napi::Function::New(env, Slide));
  exports.Set("imageBase",    Napi::Function::New(env, ImageBase));
  exports.Set("resolve",      Napi::Function::New(env, Resolve));
  exports.Set("verifyAnchor", Napi::Function::New(env, VerifyAnchor));
  return exports;
}

NODE_API_MODULE(slide, Init)
