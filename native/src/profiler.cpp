// SPDX-License-Identifier: Apache-2.0
#include "profiler.h"

#if defined(__ANDROID__)
#  include <android/trace.h>
#  include <dlfcn.h>
#endif

namespace aevum {

#if defined(__ANDROID__)

// ATrace_setCounter arrived at API 29; resolve it weakly so the library still
// loads on API 26-28 (sections there, counters only where available).
namespace {
using SetCounterFn = void (*)(const char*, int64_t);
SetCounterFn resolveSetCounter() noexcept {
  void* sym = ::dlsym(RTLD_DEFAULT, "ATrace_setCounter");
  return reinterpret_cast<SetCounterFn>(sym);
}
SetCounterFn g_set_counter = resolveSetCounter();
}  // namespace

bool traceEnabled() noexcept { return ATrace_isEnabled(); }
void traceBegin(const char* section) noexcept { ATrace_beginSection(section); }
void traceEnd() noexcept { ATrace_endSection(); }
void traceCounter(const char* name, int64_t value) noexcept {
  if (g_set_counter) g_set_counter(name, value);
}

#else  // host build: tracing is a no-op

bool traceEnabled() noexcept { return false; }
void traceBegin(const char*) noexcept {}
void traceEnd() noexcept {}
void traceCounter(const char*, int64_t) noexcept {}

#endif

}  // namespace aevum
